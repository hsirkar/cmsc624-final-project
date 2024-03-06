#include "lock_manager.h"
#include <deque>

LockManagerA::LockManagerA(deque<Txn*>* ready_txns) { ready_txns_ = ready_txns; }

bool LockManagerA::WriteLock(Txn* txn, const Key& key)
{
    // Attempts to grant a read lock to the specified transaction, enqueueing
    // request in lock table. Returns true if lock is immediately granted, else
    // returns false.
    //
    // Requires: Neither ReadLock nor WriteLock has previously been called with
    //           this txn and key.

    // if key does not exist in lock table, add it and return true
    if (lock_table_.find(key) == lock_table_.end()) {
        auto queue = deque<LockRequest>();
        lock_table_[key] = &queue;

        auto lockRequest = LockRequest(EXCLUSIVE, txn);
        lock_table_[key]->push_back(lockRequest);

        // we don't need to add this to ready_txns_ because RunLockingScheduler()
        // already does that

        return true;

    } else {
        // if key does exist, then add it to the queue
        auto lockRequest = LockRequest(EXCLUSIVE, txn);
        lock_table_[key]->push_back(lockRequest);

        // we do need to add this to txn_waits_ because RunLockingScheduler()
        // does not do that
        txn_waits_[txn]++;

        return false;
    }
}

bool LockManagerA::ReadLock(Txn* txn, const Key& key)
{
    // Since Part 1A implements ONLY exclusive locks, calls to ReadLock can
    // simply use the same logic as 'WriteLock'.
    return WriteLock(txn, key);
}

void LockManagerA::Release(Txn* txn, const Key& key)
{
    // Releases lock held by 'txn' on 'key', or cancels any pending request for
    // a lock on 'key' by 'txn'. If 'txn' held an EXCLUSIVE lock on 'key' (or was
    // the sole holder of a SHARED lock on 'key'), then the next request(s) in the
    // request queue is granted. If the granted request(s) corresponds to a
    // transaction that has now acquired ALL of its locks, that transaction is
    // appended to the 'ready_txns_' queue.
    //
    // IMPORTANT NOTE: In order to know WHEN a transaction is ready to run, you
    // may need to track its lock acquisition progress during the lock request
    // process.
    // (Hint: Use 'LockManager::txn_waits_' defined below.)

    // if key does not exist in lock table, return
    if (lock_table_.find(key) == lock_table_.end()) {
        return;
    }

    // if key does exist, then remove it from the queue
    auto queue = lock_table_[key];
    for (auto lr = queue->begin(); lr != queue->end(); lr++) {
        if (lr->txn_ == txn) {
            queue->erase(lr);
            break;
        }
    }

    // if the queue is now empty, remove the key from the lock table
    if (queue->empty()) {
        lock_table_.erase(key);
    }

    // if the txn is in txn_waits_, remove it
    if (txn_waits_.find(txn) != txn_waits_.end()) {
        txn_waits_[txn]--;
        if (txn_waits_[txn] == 0) {
            ready_txns_->push_back(txn);
            txn_waits_.erase(txn);
        }
    }
}

// NOTE: The owners input vector is NOT assumed to be empty.
LockMode LockManagerA::Status(const Key& key, vector<Txn*>* owners)
{
    // Sets '*owners' to contain the txn IDs of all txns holding the lock, and
    // returns the current LockMode of the lock: UNLOCKED if it is not currently
    // held, SHARED or EXCLUSIVE if it is, depending on the current state.

    // if key does not exist in lock table, return UNLOCKED
    if (lock_table_.find(key) == lock_table_.end()) {
        return UNLOCKED;
    }

    // if key does exist, then set owners to contain the txn IDs of all txns
    // holding the lock
    auto queue = lock_table_[key];
    for (auto lr = queue->begin(); lr != queue->end(); lr++) {
        owners->push_back(lr->txn_);
    }

    // return the current LockMode of the lock
    return queue->front().mode_;
}

LockManagerB::LockManagerB(deque<Txn*>* ready_txns) { ready_txns_ = ready_txns; }

bool LockManagerB::WriteLock(Txn* txn, const Key& key)
{
    //
    // Implement this method!
    return true;
}

bool LockManagerB::ReadLock(Txn* txn, const Key& key)
{
    //
    // Implement this method!
    return true;
}

void LockManagerB::Release(Txn* txn, const Key& key)
{
    //
    // Implement this method!
}

// NOTE: The owners input vector is NOT assumed to be empty.
LockMode LockManagerB::Status(const Key& key, vector<Txn*>* owners)
{
    //
    // Implement this method!
    return UNLOCKED;
}
