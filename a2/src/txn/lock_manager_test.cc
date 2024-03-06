#include "lock_manager.h"

#include <set>
#include <string>

#include "utils/testing.h"

#include <iostream>

using std::set;

void print_lock_table(LockManagerA* lm) {
    std::cout << "=============================" << std::endl;
    std::cout << "Lock table:" << std::endl;
    for (auto it = lm->lock_table_.begin(); it != lm->lock_table_.end(); it++) {
        std::cout << it->first << ": [";
        for (auto it2 = it->second->begin(); it2 != it->second->end(); it2++) {
            std::cout << "(" << it2->txn_ << ", " << it2->mode_ << ")" << ", ";
        }
        std::cout << "]";
        std::cout << std::endl;
    }

    std::cout << "Txn waits:" << std::endl;
    for (auto it = lm->txn_waits_.begin(); it != lm->txn_waits_.end(); it++) {
        std::cout << it->first << ": " << it->second << std::endl;
    }
    std::cout << "=============================" << std::endl;
}

TEST(LockManagerA_SimpleLocking)
{
    std::cout << "LockManagerA_SimpleLocking" << std::endl;
    deque<Txn*> ready_txns;
    LockManagerA lm(&ready_txns);
    vector<Txn*> owners;

    Txn* t1 = reinterpret_cast<Txn*>(1);
    Txn* t2 = reinterpret_cast<Txn*>(2);
    Txn* t3 = reinterpret_cast<Txn*>(3);

    std::cout << "txn1" << std::endl;
    // std::cout << "doing read lock" << std::endl;

    // Txn 1 acquires read lock.
    lm.ReadLock(t1, 101);
    print_lock_table(&lm);
    ready_txns.push_back(t1);  // Txn 1 is ready.
    EXPECT_EQ(EXCLUSIVE, lm.Status(101, &owners));
    EXPECT_EQ(1, owners.size());
    EXPECT_EQ(t1, owners[0]);
    EXPECT_EQ(1, ready_txns.size());
    EXPECT_EQ(t1, ready_txns.at(0));
    
    std::cout << "txn2" << std::endl;

    // Txn 2 requests write lock. Not granted.
    lm.WriteLock(t2, 101);
    print_lock_table(&lm);
    EXPECT_EQ(EXCLUSIVE, lm.Status(101, &owners));
    EXPECT_EQ(1, owners.size());
    EXPECT_EQ(t1, owners[0]);
    EXPECT_EQ(1, ready_txns.size());
    
    std::cout << "txn3" << std::endl;

    // Txn 3 requests read lock. Not granted.
    lm.ReadLock(t3, 101);
    print_lock_table(&lm);
    EXPECT_EQ(EXCLUSIVE, lm.Status(101, &owners));
    EXPECT_EQ(1, owners.size());
    EXPECT_EQ(t1, owners[0]);
    EXPECT_EQ(1, ready_txns.size());
    
    std::cout << "release txn1" << std::endl;

    // Txn 1 releases lock.  Txn 2 is granted write lock.
    lm.Release(t1, 101);
    print_lock_table(&lm);
    EXPECT_EQ(EXCLUSIVE, lm.Status(101, &owners));
    EXPECT_EQ(1, owners.size());
    EXPECT_EQ(t2, owners[0]);
    EXPECT_EQ(2, ready_txns.size());
    EXPECT_EQ(t2, ready_txns.at(1));

    std::cout << "release txn2" << std::endl;

    // Txn 2 releases lock.  Txn 3 is granted read lock.
    lm.Release(t2, 101);
    print_lock_table(&lm);
    EXPECT_EQ(EXCLUSIVE, lm.Status(101, &owners));
    EXPECT_EQ(1, owners.size());
    EXPECT_EQ(t3, owners[0]);
    EXPECT_EQ(3, ready_txns.size());
    EXPECT_EQ(t3, ready_txns.at(2));

    END;
}

TEST(LockManagerA_LocksReleasedOutOfOrder)
{
    deque<Txn*> ready_txns;
    LockManagerA lm(&ready_txns);
    vector<Txn*> owners;

    Txn* t1 = reinterpret_cast<Txn*>(1);
    Txn* t2 = reinterpret_cast<Txn*>(2);
    Txn* t3 = reinterpret_cast<Txn*>(3);
    Txn* t4 = reinterpret_cast<Txn*>(4);

    lm.ReadLock(t1, 101);      // Txn 1 acquires read lock.
    ready_txns.push_back(t1);  // Txn 1 is ready.
    lm.WriteLock(t2, 101);     // Txn 2 requests write lock. Not granted.
    lm.ReadLock(t3, 101);      // Txn 3 requests read lock. Not granted.
    lm.ReadLock(t4, 101);      // Txn 4 requests read lock. Not granted.

    lm.Release(t2, 101);  // Txn 2 cancels write lock request.

    // Txn 1 should now have a read lock and Txns 3 and 4 should be next in line.
    EXPECT_EQ(EXCLUSIVE, lm.Status(101, &owners));
    EXPECT_EQ(1, owners.size());
    EXPECT_EQ(t1, owners[0]);

    // Txn 1 releases lock.  Txn 3 is granted read lock.
    lm.Release(t1, 101);
    EXPECT_EQ(EXCLUSIVE, lm.Status(101, &owners));
    EXPECT_EQ(1, owners.size());
    EXPECT_EQ(t3, owners[0]);
    EXPECT_EQ(2, ready_txns.size());
    EXPECT_EQ(t3, ready_txns.at(1));

    // Txn 3 releases lock.  Txn 4 is granted read lock.
    lm.Release(t3, 101);
    EXPECT_EQ(EXCLUSIVE, lm.Status(101, &owners));
    EXPECT_EQ(1, owners.size());
    EXPECT_EQ(t4, owners[0]);
    EXPECT_EQ(3, ready_txns.size());
    EXPECT_EQ(t4, ready_txns.at(2));

    END;
}

TEST(LockManagerB_SimpleLocking)
{
    deque<Txn*> ready_txns;
    LockManagerB lm(&ready_txns);
    vector<Txn*> owners;

    Txn* t1 = reinterpret_cast<Txn*>(1);
    Txn* t2 = reinterpret_cast<Txn*>(2);
    Txn* t3 = reinterpret_cast<Txn*>(3);

    // Txn 1 acquires read lock.
    lm.ReadLock(t1, 101);
    ready_txns.push_back(t1);  // Txn 1 is ready.
    EXPECT_EQ(SHARED, lm.Status(101, &owners));
    EXPECT_EQ(1, owners.size());
    EXPECT_EQ(t1, owners[0]);
    EXPECT_EQ(1, ready_txns.size());
    EXPECT_EQ(t1, ready_txns.at(0));

    // Txn 2 requests write lock. Not granted.
    lm.WriteLock(t2, 101);
    EXPECT_EQ(SHARED, lm.Status(101, &owners));
    EXPECT_EQ(1, owners.size());
    EXPECT_EQ(t1, owners[0]);
    EXPECT_EQ(1, ready_txns.size());

    // Txn 3 requests read lock. Not granted.
    lm.ReadLock(t3, 101);
    EXPECT_EQ(SHARED, lm.Status(101, &owners));
    EXPECT_EQ(1, owners.size());
    EXPECT_EQ(t1, owners[0]);
    EXPECT_EQ(1, ready_txns.size());

    // Txn 1 releases lock.  Txn 2 is granted write lock.
    lm.Release(t1, 101);
    EXPECT_EQ(EXCLUSIVE, lm.Status(101, &owners));
    EXPECT_EQ(1, owners.size());
    EXPECT_EQ(t2, owners[0]);
    EXPECT_EQ(2, ready_txns.size());
    EXPECT_EQ(t2, ready_txns.at(1));

    // Txn 2 releases lock.  Txn 3 is granted read lock.
    lm.Release(t2, 101);
    EXPECT_EQ(SHARED, lm.Status(101, &owners));
    EXPECT_EQ(1, owners.size());
    EXPECT_EQ(t3, owners[0]);
    EXPECT_EQ(3, ready_txns.size());
    EXPECT_EQ(t3, ready_txns.at(2));

    END;
}

TEST(LockManagerB_LocksReleasedOutOfOrder)
{
    deque<Txn*> ready_txns;
    LockManagerB lm(&ready_txns);
    vector<Txn*> owners;

    Txn* t1 = reinterpret_cast<Txn*>(1);
    Txn* t2 = reinterpret_cast<Txn*>(2);
    Txn* t3 = reinterpret_cast<Txn*>(3);
    Txn* t4 = reinterpret_cast<Txn*>(4);

    lm.ReadLock(t1, 101);      // Txn 1 acquires read lock.
    ready_txns.push_back(t1);  // Txn 1 is ready.
    lm.WriteLock(t2, 101);     // Txn 2 requests write lock. Not granted.
    lm.ReadLock(t3, 101);      // Txn 3 requests read lock. Not granted.
    lm.ReadLock(t4, 101);      // Txn 4 requests read lock. Not granted.

    lm.Release(t2, 101);  // Txn 2 cancels write lock request.

    // Txns 1, 3 and 4 should now have a shared lock.
    EXPECT_EQ(SHARED, lm.Status(101, &owners));
    EXPECT_EQ(3, owners.size());
    EXPECT_EQ(t1, owners[0]);
    EXPECT_EQ(t3, owners[1]);
    EXPECT_EQ(t4, owners[2]);
    EXPECT_EQ(3, ready_txns.size());
    EXPECT_EQ(t1, ready_txns.at(0));
    EXPECT_EQ(t3, ready_txns.at(1));
    EXPECT_EQ(t4, ready_txns.at(2));

    END;
}

int main(int argc, char** argv)
{
    std::cout << "A. Testing simple locking..." << std::endl;
    LockManagerA_SimpleLocking();
    std::cout << "A. Testing locks released out of order..." << std::endl;
    LockManagerA_LocksReleasedOutOfOrder();
    std::cout << "B. Testing simple locking..." << std::endl;
    LockManagerB_SimpleLocking();
    std::cout << "B. Testing locks released out of order..." << std::endl;
    LockManagerB_LocksReleasedOutOfOrder();
}
