#include "txn_processor.h"
#include <chrono>
#include <set>
#include <stdio.h>
#include <unordered_set>

#include "lock_manager.h"

// Thread & queue counts for StaticThreadPool initialization.
#define THREAD_COUNT 8

TxnProcessor::TxnProcessor(CCMode mode)
    : mode_(mode), tp_(THREAD_COUNT), next_unique_id_(1) {
  if (mode_ == LOCKING_EXCLUSIVE_ONLY)
    lm_ = new LockManagerA(&ready_txns_);
  else if (mode_ == LOCKING)
    lm_ = new LockManagerB(&ready_txns_);

  // Create the storage
  if (mode_ == MVCC || mode_ == MVCC_SSI) {
    storage_ = new MVCCStorage();
  } else {
    storage_ = new Storage();
  }

  storage_->InitStorage();

  // Start 'RunScheduler()' running.

  pthread_attr_t attr;
  pthread_attr_init(&attr);

#if !defined(_MSC_VER) && !defined(__APPLE__)
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  for (int i = 0; i < 7; i++) {
    CPU_SET(i, &cpuset);
  }
  pthread_attr_setaffinity_np(&attr, sizeof(cpu_set_t), &cpuset);
#endif

  pthread_t scheduler_;
  pthread_create(&scheduler_, &attr, StartScheduler,
                 reinterpret_cast<void *>(this));

  stopped_ = false;
  scheduler_thread_ = scheduler_;
}

void *TxnProcessor::StartScheduler(void *arg) {
  reinterpret_cast<TxnProcessor *>(arg)->RunScheduler();
  return NULL;
}

TxnProcessor::~TxnProcessor() {
  // Wait for the scheduler thread to join back before destroying the object and
  // its thread pool.
  stopped_ = true;
  pthread_join(scheduler_thread_, NULL);
  if(calvin_epoch_executor_thread != NULL)
    pthread_join(calvin_epoch_executor_thread, NULL);
  if(calvin_sequencer_thread != NULL)
    pthread_join(calvin_sequencer_thread, NULL);

  if (mode_ == LOCKING_EXCLUSIVE_ONLY || mode_ == LOCKING)
    delete lm_;

  delete storage_;
}

void TxnProcessor::NewTxnRequest(Txn *txn) {
  // Atomically assign the txn a new number and add it to the incoming txn
  // requests queue.
  mutex_.Lock();
  txn->unique_id_ = next_unique_id_;
  next_unique_id_++;
  txn_requests_.Push(txn);
  mutex_.Unlock();
}

Txn *TxnProcessor::GetTxnResult() {
  Txn *txn;
  while (!txn_results_.Pop(&txn)) {
    // No result yet. Wait a bit before trying again (to reduce contention on
    // atomic queues).
    usleep(1);
  }
  return txn;
}

void TxnProcessor::RunScheduler() {
  switch (mode_) {
  case SERIAL:
    RunSerialScheduler();
    break;
  case LOCKING:
    RunLockingScheduler();
    break;
  case LOCKING_EXCLUSIVE_ONLY:
    RunLockingScheduler();
    break;
  case OCC:
    RunOCCScheduler();
    break;
  case P_OCC:
    RunOCCParallelScheduler();
    break;
  case MVCC:
    RunMVCCScheduler();
    break;
  case CALVIN:
    RunCalvinScheduler();
    break;
  case CALVIN_EPOCH:
    RunCalvinEpochScheduler();
  }
}

void TxnProcessor::RunSerialScheduler() {
  Txn *txn;
  while (!stopped_) {
    // Get next txn request.
    if (txn_requests_.Pop(&txn)) {
      // Execute txn.
      ExecuteTxn(txn);

      // Commit/abort txn according to program logic's commit/abort decision.
      if (txn->Status() == COMPLETED_C) {
        ApplyWrites(txn);
        committed_txns_.Push(txn);
        txn->status_ = COMMITTED;
      } else if (txn->Status() == COMPLETED_A) {
        txn->status_ = ABORTED;
      } else {
        // Invalid TxnStatus!
        DIE("Completed Txn has invalid TxnStatus: " << txn->Status());
      }

      // Return result to client.
      txn_results_.Push(txn);
    }
  }
}

void TxnProcessor::RunLockingScheduler() {
  Txn *txn;

  while (!stopped_) {
    // Start processing the next incoming transaction request.
    if (txn_requests_.Pop(&txn)) {
      bool blocked = false;
      // Request read locks.
      for (set<Key>::iterator it = txn->readset_.begin();
           it != txn->readset_.end(); ++it) {
        if (!lm_->ReadLock(txn, *it)) {
          blocked = true;
        }
      }

      // Request write locks.
      for (set<Key>::iterator it = txn->writeset_.begin();
           it != txn->writeset_.end(); ++it) {
        if (!lm_->WriteLock(txn, *it)) {
          blocked = true;
        }
      }

      // If all read and write locks were immediately acquired, this txn is
      // ready to be executed.
      if (blocked == false) {
        ready_txns_.push_back(txn);
      }
    }

    // Process and commit all transactions that have finished running.
    while (completed_txns_.Pop(&txn)) {
      // Commit/abort txn according to program logic's commit/abort decision.
      if (txn->Status() == COMPLETED_C) {
        ApplyWrites(txn);
        committed_txns_.Push(txn);
        txn->status_ = COMMITTED;
      } else if (txn->Status() == COMPLETED_A) {
        txn->status_ = ABORTED;
      } else {
        // Invalid TxnStatus!
        DIE("Completed Txn has invalid TxnStatus: " << txn->Status());
      }

      // Release read locks.
      for (set<Key>::iterator it = txn->readset_.begin();
           it != txn->readset_.end(); ++it) {
        lm_->Release(txn, *it);
      }
      // Release write locks.
      for (set<Key>::iterator it = txn->writeset_.begin();
           it != txn->writeset_.end(); ++it) {
        lm_->Release(txn, *it);
      }

      // Return result to client.
      txn_results_.Push(txn);
    }

    // Start executing all transactions that have newly acquired all their
    // locks.
    while (ready_txns_.size()) {
      // Get next ready txn from the queue.
      txn = ready_txns_.front();
      ready_txns_.pop_front();

      // Start txn running in its own thread.
      tp_.AddTask([this, txn]() { this->ExecuteTxn(txn); });
    }
  }
}

void TxnProcessor::ExecuteTxn(Txn *txn) {
  // Get the current commited transaction index for the further validation.
  txn->occ_start_idx_ = committed_txns_.Size();

  // Read everything in from readset.
  for (set<Key>::iterator it = txn->readset_.begin(); it != txn->readset_.end();
       ++it) {
    // Save each read result iff record exists in storage.
    Value result;
    if (storage_->Read(*it, &result))
      txn->reads_[*it] = result;
  }

  // Also read everything in from writeset.
  for (set<Key>::iterator it = txn->writeset_.begin();
       it != txn->writeset_.end(); ++it) {
    // Save each read result iff record exists in storage.
    Value result;
    if (storage_->Read(*it, &result))
      txn->reads_[*it] = result;
  }

  // Execute txn's program logic.
  txn->Run();

  // Hand the txn back to the RunScheduler thread.
  completed_txns_.Push(txn);
}

void TxnProcessor::RunCalvinSequencer() {
  Txn *txn;
  // save time of last epoch for calvin sequencer
  auto last_epoch_time = std::chrono::high_resolution_clock::now();
  // set up current epoch
  Epoch *current_epoch = new Epoch();
  while (!stopped_) {
    // Add the txn to the epoch.
    if (txn_requests_.Pop(&txn)) {
      current_epoch->push(txn);
    }

    // check if we need to close the epoch
    auto curr_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        curr_time - last_epoch_time);
    if (duration.count() > 10) {
      // new epoch is out of scope
      last_epoch_time = curr_time;

      // make new epoch if last epoch has anything in it
      if (!current_epoch->empty()) {
        epoch_queue.Push(current_epoch);
        current_epoch = new Epoch();
      }
    }
  }
}

void *TxnProcessor::calvin_sequencer_helper(void *arg) {
  reinterpret_cast<TxnProcessor *>(arg)->RunCalvinSequencer();
  return NULL;
}

void *TxnProcessor::calvin_epoch_executor_helper(void *arg) {
  reinterpret_cast<TxnProcessor *>(arg)->CalvinEpochExecutor();
  return NULL;
}

void TxnProcessor::ExecuteTxnCalvin(Txn *txn) {
  // Execute txn.
  ExecuteTxn(txn);

  // Commit/abort txn according to program logic's commit/abort decision.
  // Note: we do this within the worker thread instead of returning
  // back to the scheduler thread.
  if (txn->Status() == COMPLETED_C) {
    ApplyWrites(txn);
    committed_txns_.Push(txn);
    txn->status_ = COMMITTED;
  } else if (txn->Status() == COMPLETED_A) {
    txn->status_ = ABORTED;
  } else {
    // Invalid TxnStatus!
    DIE("Completed Txn has invalid TxnStatus: " << txn->Status());
  }

  // Return result to client.
  txn_results_.Push(txn);
}

void TxnProcessor::RunCalvinScheduler() {
  Txn *txn;

  while (!stopped_) {
    // Get the next new transaction request (if one is pending) and pass it to
    // an execution thread that executes the txn logic *and also* does the
    // validation and write phases.
    if (txn_requests_.Pop(&txn)) {
      // ...
    }
  }
}

void TxnProcessor::ExecuteTxnCalvinEpoch(Txn *txn) {
  // Execute txn.
  ExecuteTxn(txn);

  // Commit/abort txn according to program logic's commit/abort decision.
  // Note: we do this within the worker thread instead of returning
  // back to the scheduler thread.
  if (txn->Status() == COMPLETED_C) {
    ApplyWrites(txn);
    committed_txns_.Push(txn);
    txn->status_ = COMMITTED;
  } else if (txn->Status() == COMPLETED_A) {
    txn->status_ = ABORTED;
  } else {
    // Invalid TxnStatus!
    DIE("Completed Txn has invalid TxnStatus: " << txn->Status());
  }

  // update number of transactions left and signal if finished
//  if (num_txns_left_in_epoch == 1) {
//    num_txns_left_in_epoch = 0;
////    pthread_cond_signal(&epoch_finished_cond);
//  } else {
//    num_txns_left_in_epoch--;
//  }
  num_txns_left_in_epoch--;

  // Update indegrees of neighbors
  // If any has indegree 0, add them back to the queue
  auto neighbors = current_epoch_dag->adj_list->at(txn);
  for (auto blocked_txn : neighbors) {
    current_epoch_dag->indegree->at(blocked_txn)--;
    if (current_epoch_dag->indegree->at(blocked_txn) == 0) {
      tp_.AddTask([this, txn]() { this->ExecuteTxnCalvinEpoch(txn); });
    }
  }

  // Return result to client.
  txn_results_.Push(txn);
}

void TxnProcessor::RunCalvinEpochScheduler() {
  // set up mutexes
//  pthread_mutex_init(&epoch_finished_mutex, NULL);
//  pthread_cond_init(&epoch_finished_cond, NULL);
//  epoch_finished_mutex = PTHREAD_MUTEX_INITIALIZER;
//  epoch_finished_cond = PTHREAD_COND_INITIALIZER;
  // Start Calvin Sequencer
  pthread_create(&calvin_sequencer_thread, NULL, calvin_sequencer_helper,
                 reinterpret_cast<void *>(this));
  // Start Calvin Epoch Executor
  pthread_create(&calvin_epoch_executor_thread, NULL, calvin_epoch_executor_helper,
                 reinterpret_cast<void *>(this));



  Epoch *curr_epoch;
  EpochDag *dag;
  while (!stopped_) {
    // Get the next epoch
    if (epoch_queue.Pop(&curr_epoch)) {
      // Create new DAG for this epoch
      std::unordered_map<Key, std::unordered_set<Txn *>> shared_holders;
      std::unordered_map<Key, Txn *> last_excl;

      dag = (EpochDag *)malloc(sizeof(EpochDag));

      std::unordered_map<Txn *, std::unordered_set<Txn *>>* adj_list = new std::unordered_map<Txn *, std::unordered_set<Txn *>>();
      std::unordered_map<Txn *, std::atomic<int>>* indegree = new std::unordered_map<Txn *, std::atomic<int>>();
      std::queue<Txn *>* root_txns = new std::queue<Txn *>();

      Txn *txn;
      while (!curr_epoch->empty()) {
        txn = curr_epoch->front();
        curr_epoch->pop();
        adj_list->emplace(txn, std::unordered_set<Txn *>());
        indegree->emplace(txn, 0);

        // Loop through readset
        for (const Key &key : txn->readset_) {
          // Add to shared holders
          if (!shared_holders.contains(key)) {
            shared_holders[key] = std::unordered_set<Txn *>();
          }
          shared_holders[key].insert(txn);

          // If the last_excl txn is not the current txn, add an edge
          if (last_excl.contains(key) && last_excl[key] != txn &&
              !adj_list->at(last_excl[key]).contains(txn)) {
            adj_list->at(last_excl[key]).insert(txn);
            indegree->at(txn)++;
          }
        }
        // Loop through writeset
        for (const Key &key : txn->writeset_) {
          // Add an edge between the current txn and all shared holders
          if (shared_holders.contains(key)) {
            for (auto conflicting_txn : shared_holders[key]) {
              if (conflicting_txn != txn &&
                  !adj_list->at(conflicting_txn).contains(txn)) {
                adj_list->at(conflicting_txn).insert(txn);
                indegree->at(txn)++;
              }
            }
            shared_holders[key].clear();
          }
          last_excl[key] = txn;
        }

        // set as root if indegree of 0
        if (indegree->at(txn) == 0) {
          root_txns->push(txn);
        }
      }
      // finalize new epoch dag
      dag->adj_list = adj_list;
      dag->indegree = indegree;
      dag->root_txns = root_txns;

      // push dag to queue for executor to read
      epoch_dag_queue.Push(dag);
    }
  }
}

void TxnProcessor::CalvinEpochExecutor() {
  EpochDag *current_epoch;
  while (!stopped_) {
    if (epoch_dag_queue.Pop(&current_epoch)) {
      current_epoch_dag = current_epoch;
      num_txns_left_in_epoch = current_epoch->adj_list->size();
      Txn *txn;
      std::queue<Txn *> *root_txns = current_epoch->root_txns;

      // add all root txns to threadpool
      while (!root_txns->empty()) {
        txn = root_txns->front();
        root_txns->pop();
        tp_.AddTask([this, txn]() { this->ExecuteTxnCalvinEpoch(txn); });
      }

      // wait for epoch to end executing
      int sleep_duration = 1; // in microseconds
      while (num_txns_left_in_epoch > 0) {
        usleep(sleep_duration);
        // Back off exponentially.
        if (sleep_duration < 32)
          sleep_duration *= 2;
      }
    }
  }
}

void TxnProcessor::ApplyWrites(Txn *txn) {
  // Write buffered writes out to storage.
  for (map<Key, Value>::iterator it = txn->writes_.begin();
       it != txn->writes_.end(); ++it) {
    storage_->Write(it->first, it->second, txn->unique_id_);
  }
}

void TxnProcessor::RunOCCScheduler() {
  Txn *txn;
  while (!stopped_) {
    // Get the next new txn request (if one is pending)
    if (txn_requests_.Pop(&txn)) {
      // Pass it to an execution thread
      tp_.AddTask([this, txn]() { this->ExecuteTxn(txn); });
    }

    // Dealing with a finished transaction
    while (completed_txns_.Pop(&txn)) {
      // Validation phase
      // Use the data structure in `txn_processor` class to check overlap with
      // each record whose key appears in the txn's read and write sets
      bool valid = true;

      // Check for overlap with newly committed transactions
      // after the txn's occ_start_idx_
      for (int i = txn->occ_start_idx_ + 1; i < committed_txns_.Size(); i++) {
        Txn *t = committed_txns_[i];

        // check if write_set of t intersects with read_set of txn
        for (auto key : txn->readset_) {
          if (t->writeset_.find(key) != t->writeset_.end()) {
            valid = false;
            break;
          }
        }
      }

      // If validation failed, cleanup txn and completely restart it
      if (!valid) {
        // Cleanup txn
        txn->reads_.clear();
        txn->writes_.clear();
        txn->status_ = INCOMPLETE;

        // Restart txn
        mutex_.Lock();
        txn->unique_id_ = next_unique_id_;
        next_unique_id_++;
        txn_requests_.Push(txn);
        mutex_.Unlock();
      } else {
        // Apply all writes
        ApplyWrites(txn);

        // Mark transaction as committed
        committed_txns_.Push(txn);
        txn->status_ = COMMITTED;

        // Update relevant data structure
        txn_results_.Push(txn);
      }
    }
  }
}

void TxnProcessor::ExecuteTxnParallel(Txn *txn) {
  // Note that you can use active_set_ and active_set_mutex_ we provided
  // for you in the txn_processor.h

  // Record start time
  txn->occ_start_idx_ = committed_txns_.Size();

  // Perform "read phase" of transaction
  // Read everything in from readset.
  for (set<Key>::iterator it = txn->readset_.begin(); it != txn->readset_.end();
       ++it) {
    // Save each read result iff record exists in storage.
    Value result;
    if (storage_->Read(*it, &result))
      txn->reads_[*it] = result;
  }

  // Also read everything in from writeset.
  for (set<Key>::iterator it = txn->writeset_.begin();
       it != txn->writeset_.end(); ++it) {
    // Save each read result iff record exists in storage.
    Value result;
    if (storage_->Read(*it, &result))
      txn->reads_[*it] = result;
  }

  // Execute txn's program logic.
  txn->Run();

  // Start of critical section
  active_set_mutex_.Lock();

  // Make a copy of the active set
  auto finish_active = active_set_.GetSet();

  // Add this txn to the active set
  active_set_.Insert(txn);

  // End of critical section
  active_set_mutex_.Unlock();

  // Validation phase
  // Use the data structure in `txn_processor` class to check overlap with
  // each record whose key appears in the txn's read and write sets
  bool valid = true;

  // NOTE: This is not in the pseudocode in the project description
  // Check for overlap with newly committed transactions
  // after the txn's occ_start_idx_
  for (int i = txn->occ_start_idx_ + 1; i < committed_txns_.Size(); i++) {
    Txn *t = committed_txns_[i];

    // check if write_set of t intersects with read_set of txn
    for (auto key : txn->readset_) {
      if (t->writeset_.find(key) != t->writeset_.end()) {
        valid = false;
        break;
      }
    }
  }

  // Check overlap with each record whose key appears in the txn's read and
  // write sets NOTE: we only run this if the txn hasn't been invalidated by the
  // previous check NOTE: this is the only validation implemented in the
  // pseudocode in the project description
  if (valid) {
    for (auto t : finish_active) {
      // if txn's write set intersects with t's write sets
      for (auto key : txn->writeset_) {
        if (t->writeset_.find(key) != t->writeset_.end()) {
          valid = false;
          break;
        }
      }

      // if txn's read set intersects with t's write sets
      for (auto key : txn->readset_) {
        if (t->writeset_.find(key) != t->writeset_.end()) {
          valid = false;
          break;
        }
      }
    }
  }

  // If validation failed, cleanup txn and completely restart it
  if (!valid) {
    // Remove this txn from the active set
    active_set_mutex_.Lock();
    active_set_.Erase(txn);
    active_set_mutex_.Unlock();

    // Cleanup txn
    txn->reads_.clear();
    txn->writes_.clear();
    txn->status_ = INCOMPLETE;

    // Restart txn
    mutex_.Lock();
    txn->unique_id_ = next_unique_id_;
    next_unique_id_++;
    txn_requests_.Push(txn);
    mutex_.Unlock();
  } else {
    // Apply all writes
    ApplyWrites(txn);

    // Remove this txn from the active set
    active_set_mutex_.Lock();
    active_set_.Erase(txn);
    active_set_mutex_.Unlock();

    // Mark transaction as committed
    committed_txns_.Push(txn);
    txn->status_ = COMMITTED;

    // Update relevant data structure
    txn_results_.Push(txn);
  }
}

void TxnProcessor::RunOCCParallelScheduler() {
  //
  // Implement this method! Note that implementing OCC with parallel
  // validation may need to create another method, like
  // TxnProcessor::ExecuteTxnParallel.
  // Note that you can use active_set_ and active_set_mutex_ we provided
  // for you in the txn_processor.h

  Txn *txn;
  while (!stopped_) {
    // Get the next new transaction request (if one is pending) and pass it to
    // an execution thread that executes the txn logic *and also* does the
    // validation and write phases.
    if (txn_requests_.Pop(&txn)) {
      tp_.AddTask([this, txn]() { this->ExecuteTxnParallel(txn); });
    }
  }
}

// Helper function to take the union of two sets
set<Key> set_union(const set<Key> &s1, const set<Key> &s2) {
  set<Key> result = s1;
  result.insert(s2.begin(), s2.end());
  return result;
}

void TxnProcessor::MVCCExecuteTxn(Txn *txn) {
  // Read all necessary data for this transaction from storage
  // (Note that unlike the version of MVCC from class, you should lock the key
  // before each read)

  // Read everything in from readset and writeset.
  for (auto key : set_union(txn->readset_, txn->writeset_)) {
    // Lock the key
    storage_->Lock(key);

    // Save each read result iff record exists in storage.
    Value result;
    if (storage_->Read(key, &result, txn->unique_id_))
      txn->reads_[key] = result;

    // Unlock the key
    storage_->Unlock(key);
  }

  // Execute txn's program logic.
  txn->Run();

  // Acquire all locks for keys in the write_set_
  for (auto key : txn->writeset_) {
    storage_->Lock(key);
  }

  // Call MVCCStorage::CheckWrite method to check all keys in the write_set_
  bool checkPassed = true;
  for (auto key : txn->writeset_) {
    if (!((MVCCStorage *)storage_)->CheckKey(key, txn->unique_id_)) {
      checkPassed = false;
      break;
    }
  }

  // If each key passed the check
  if (checkPassed) {
    // Apply the writes
    ApplyWrites(txn);

    // Release all locks for keys in the write_set_
    for (auto key : txn->writeset_) {
      storage_->Unlock(key);
    }

    // Mark transaction as committed
    committed_txns_.Push(txn);
    txn->status_ = COMMITTED;

    // Update relevant data structure
    txn_results_.Push(txn);
  } else { // At least one key failed the check
    // Release all locks for keys in the write_set_
    for (auto key : txn->writeset_) {
      storage_->Unlock(key);
    }

    // Cleanup txn
    txn->reads_.clear();
    txn->writes_.clear();
    txn->status_ = INCOMPLETE;

    // Restart txn -- same as OCC
    mutex_.Lock();
    txn->unique_id_ = next_unique_id_;
    next_unique_id_++;
    txn_requests_.Push(txn);
    mutex_.Unlock();
  }
}

void TxnProcessor::RunMVCCScheduler() {
  //
  // Implement this method!

  // Hint:Pop a txn from txn_requests_, and pass it to a thread to execute.
  // Note that you may need to create another execute method, like
  // TxnProcessor::MVCCExecuteTxn.

  Txn *txn;
  while (!stopped_) {
    // Get the next new transaction request (if one is pending) and pass it to
    // an execution thread that executes the txn logic *and also* does the
    // validation and write phases.
    if (txn_requests_.Pop(&txn)) {
      tp_.AddTask([this, txn]() { this->MVCCExecuteTxn(txn); });
    }
  }
}

void TxnProcessor::MVCCSSIExecuteTxn(Txn *txn) {
  // Read all necessary data for this transaction from storage
  // (Note that unlike the version of MVCC from class, you should lock the key
  // before each read)

  // Read everything in from readset and writeset.
  for (auto key : set_union(txn->readset_, txn->writeset_)) {
    // Lock the key
    storage_->Lock(key);

    // Save each read result iff record exists in storage.
    Value result;
    if (storage_->Read(key, &result, txn->unique_id_))
      txn->reads_[key] = result;

    // Unlock the key
    storage_->Unlock(key);
  }

  // Execute txn's program logic.
  txn->Run();

  // THIS IS DIFFERENT FROM MVCCExecuteTxn: we lock write_set AND read_set
  // Acquire all locks for keys in the read_set_ and write_set_
  // (Lock any overlapping key only once.)
  for (auto key : set_union(txn->writeset_, txn->readset_)) {
    storage_->Lock(key);
  }

  // Call MVCCStorage::CheckWrite method to check all keys in the write_set_
  bool checkPassed = true;
  for (auto key : txn->writeset_) {
    if (!((MVCCStorage *)storage_)->CheckKey(key, txn->unique_id_)) {
      checkPassed = false;
      break;
    }
  }

  // If each key passed the check
  if (checkPassed) {
    // Apply the writes
    ApplyWrites(txn);

    // Release all locks for ALL keys (read_set_ and write_set_)
    for (auto key : set_union(txn->writeset_, txn->readset_)) {
      storage_->Unlock(key);
    }

    // Mark transaction as committed
    committed_txns_.Push(txn);
    txn->status_ = COMMITTED;

    // Update relevant data structure
    txn_results_.Push(txn);
  } else { // At least one key failed the check
    // Release all locks for ALL keys (read_set_ and write_set_)
    for (auto key : set_union(txn->writeset_, txn->readset_)) {
      storage_->Unlock(key);
    }

    // Cleanup txn
    txn->reads_.clear();
    txn->writes_.clear();
    txn->status_ = INCOMPLETE;

    // Restart txn -- same as OCC
    mutex_.Lock();
    txn->unique_id_ = next_unique_id_;
    next_unique_id_++;
    txn_requests_.Push(txn);
    mutex_.Unlock();
  }
}

void TxnProcessor::RunMVCCSSIScheduler() {
  //
  // Implement this method!

  // Hint:Pop a txn from txn_requests_, and pass it to a thread to execute.
  // Note that you may need to create another execute method, like
  // TxnProcessor::MVCCSSIExecuteTxn.

  Txn *txn;
  while (!stopped_) {
    // Get the next new transaction request (if one is pending) and pass it to
    // an execution thread that executes the txn logic *and also* does the
    // validation and write phases.
    if (txn_requests_.Pop(&txn)) {
      tp_.AddTask([this, txn]() { this->MVCCSSIExecuteTxn(txn); });
    }
  }
}
