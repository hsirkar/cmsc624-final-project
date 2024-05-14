#include "txn_processor.h"
#include "utils/common.h"
#include <chrono>
#include <set>
#include <stdio.h>
#include <unordered_set>

#include "lock_manager.h"

/***********************************************
 *  Calvin Continuous Execution -- Global Locks *
 ***********************************************/
void TxnProcessor::RunCalvinContScheduler() {}

void TxnProcessor::CalvinContExecutorFunc() {
  std::cout << "Calvin Continuous Executor Function" << std::endl;
}

/***********************************************
 *  Calvin Continuous Execution -- Indiv Locks  *
 ***********************************************/
void TxnProcessor::RunCalvinContIndivScheduler() {}

void TxnProcessor::CalvinContIndivExecutorFunc() {
  std::cout << "Calvin Continuous Individual Executor Function" << std::endl;
}

/***********************************************
 *            Calvin Epoch Execution            *
 ***********************************************/

void TxnProcessor::RunCalvinEpochSequencer() {
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
    if (duration.count() > 5) {
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
  reinterpret_cast<TxnProcessor *>(arg)->RunCalvinEpochSequencer();
  return NULL;
}

void TxnProcessor::RunCalvinEpochScheduler() {

  // Start Calvin Sequencer
  pthread_create(&calvin_sequencer_thread, NULL, calvin_sequencer_helper,
                 reinterpret_cast<void *>(this));
  //   Start Calvin Epoch Executor
  pthread_create(&calvin_epoch_executor_thread, NULL,
                 calvin_epoch_executor_helper, reinterpret_cast<void *>(this));
  // for(int i = 0; i < THREAD_COUNT; i++) {
  //   tp_.AddTask([this]() { this->CalvinEpochExecutorLMAO(); });
  // }

  Epoch *curr_epoch;
  EpochDag *dag;
  while (!stopped_) {
    // Get the next epoch
    if (epoch_queue.Pop(&curr_epoch)) {
      // Create new DAG for this epoch
      std::unordered_map<Key, std::unordered_set<Txn *>> shared_holders;
      std::unordered_map<Key, Txn *> last_excl;

      dag = (EpochDag *)malloc(sizeof(EpochDag));

      std::unordered_map<Txn *, std::unordered_set<Txn *>> *adj_list =
          new std::unordered_map<Txn *, std::unordered_set<Txn *>>();
      std::unordered_map<Txn *, std::atomic<int>> *indegree =
          new std::unordered_map<Txn *, std::atomic<int>>();
      std::queue<Txn *> *root_txns = new std::queue<Txn *>();

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

          // add an edge between the last_excl txn and the current txn
          // if we read a key that was last written by last_excl txn
          if (last_excl.contains(key) &&
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
              if (!adj_list->at(conflicting_txn).contains(txn)) {
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
      //      dag->indegree_locks = indegree_locks;

      // push dag to queue for executor to read
      epoch_dag_queue.Push(dag);
    }
  }
}

void TxnProcessor::CalvinEpochExecutor() {
  EpochDag *current_epoch;
  num_txns_left_in_epoch = 0;
  while (!stopped_) {
    if (epoch_dag_queue.Pop(&current_epoch)) {
      if (num_txns_left_in_epoch != 0) {
        std::cout << "Num transactions in epoch: " << num_txns_left_in_epoch
                  << std::endl;
        std::cout << "UH OH--------------------------------UH OH" << std::endl;
      }
      current_epoch_dag = current_epoch;
      num_txns_left_in_epoch = current_epoch->adj_list->size();
      Txn *txn;
      std::queue<Txn *> *root_txns = current_epoch->root_txns;

      // add all root txns to threadpool
      while (!root_txns->empty()) {
        txn = root_txns->front();
        root_txns->pop();
        tp_.AddTask([this, txn]() { this->ExecuteTxnCalvinEpoch(txn); });
        // calvin_ready_txns_.Push(txn);
        //        calvin_ready_txns_.
      }

      // wait for epoch to end executing
      int sleep_duration = 1; // in microseconds
      while (num_txns_left_in_epoch > 0) {
        usleep(sleep_duration);
        // Back off exponentially.
        if (sleep_duration < 32)
          sleep_duration *= 2;
      }
      delete current_epoch->adj_list;
      delete current_epoch->indegree;
      delete current_epoch->root_txns;
      free(current_epoch);
    }
  }
}

void *TxnProcessor::calvin_epoch_executor_helper(void *arg) {
  reinterpret_cast<TxnProcessor *>(arg)->CalvinEpochExecutor();
  return NULL;
}

void TxnProcessor::CalvinEpochExecutorFunc() {
  Txn *txn;
  while (!stopped_) {
    // Get the next new transaction request (if one is pending) and pass it to
    // an execution thread that executes the txn logic *and also* does the
    // validation and write phases.
    if (calvin_ready_txns_.Pop(&txn)) {
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

      // Update indegrees of neighbors
      // If any has indegree 0, add them back to the queue
      if (num_txns_left_in_epoch-- > 1) {
        auto neighbors = current_epoch_dag->adj_list->at(txn);
        for (Txn *blocked_txn : neighbors) {
          if (current_epoch_dag->indegree->at(blocked_txn)-- == 1) {
            calvin_ready_txns_.Push(blocked_txn);
          }
        }
      }

      // Return result to client.
      txn_results_.Push(txn);
    }
  }
}
