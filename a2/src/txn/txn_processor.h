#ifndef _TXN_PROCESSOR_H_
#define _TXN_PROCESSOR_H_

#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "lock_manager.h"
#include "mvcc_storage.h"
#include "storage.h"
#include "txn.h"
#include "utils/atomic.h"
#include "utils/common.h"
#include "utils/mutex.h"
#include "utils/static_thread_pool.h"

using std::deque;
using std::map;
using std::string;

// The TxnProcessor supports six different execution modes, corresponding to
// the four parts of assignment 2, plus a simple serial (non-concurrent) mode,
// plus Calvin (as a part of the final project).
enum CCMode {
  SERIAL = 0,                 // Serial transaction execution (no concurrency)
  LOCKING_EXCLUSIVE_ONLY = 1, // Part 1A
  LOCKING = 2,                // Part 1B
  OCC = 3,                    // Part 2
  P_OCC = 4,                  // Part 3
  MVCC = 5,                   // Part 4
  MVCC_SSI = 6,               // Part 5
  CALVIN_CONT = 7,
  CALVIN_CONT_INDIV = 8,
  CALVIN_EPOCH = 9,
};

// Returns a human-readable string naming of the providing mode.
string ModeToString(CCMode mode);

class TxnProcessor {
public:
  // The TxnProcessor's constructor starts the TxnProcessor running in the
  // background.
  explicit TxnProcessor(CCMode mode);

  // The TxnProcessor's destructor stops all background threads and deallocates
  // all objects currently owned by the TxnProcessor, except for Txn objects.
  ~TxnProcessor();

  // Registers a new txn request to be executed by the TxnProcessor.
  // Ownership of '*txn' is transfered to the TxnProcessor.
  void NewTxnRequest(Txn *txn);

  // Returns a pointer to the next COMMITTED or ABORTED Txn. The caller takes
  // ownership of the returned Txn.
  Txn *GetTxnResult();

  // Main loop implementing all concurrency control/thread scheduling.
  void RunScheduler();

  static void *StartScheduler(void *arg);

  // putting calvin sequencer as public for pthread
  void RunCalvinEpochSequencer();

  // putting calvin epoch executor as public for pthread
  void CalvinEpochExecutor();

private:

  // ===================== START OF CALVIN =====================

  /***********************************************
  *  Calvin Continuous Execution -- Global Locks *
  ***********************************************/
  std::unordered_map<Txn *, std::unordered_set<Txn *>> adj_list;
  std::unordered_map<Txn *, std::atomic<int>>
      indegree; // indegree needs to be atomic
  std::queue<Txn *> *root_txns;

  std::shared_mutex adj_list_lock;
  std::shared_mutex indegree_lock;

  void RunCalvinContScheduler();
  void CalvinContExecutorFunc();

  /***********************************************
  *  Calvin Continuous Execution -- Indiv Locks  *
  ***********************************************/
  void RunCalvinContIndivScheduler();
  void CalvinContIndivExecutorFunc();

  /***********************************************
  *            Calvin Epoch Execution            *
  ***********************************************/
  // 1) Sequencer

  // thread for calvin sequencer
  pthread_t calvin_sequencer_thread;
  // thread for calvin sequencer
  pthread_t calvin_epoch_executor_thread;
  // defining epoch for ease of use
  typedef std::queue<Txn *> Epoch;
  // queue of epochs for calvin scheduler
  AtomicQueue<Epoch *> epoch_queue;
  
  // void RunCalvinEpochSequencer();
  // helper function for pthreads
  static void *calvin_sequencer_helper(void *arg);

  // 2) Scheduler
  struct EpochDag {
    std::unordered_map<Txn *, std::unordered_set<Txn *>> *adj_list;
    std::unordered_map<Txn *, std::atomic<int>> *indegree;
    std::queue<Txn *> *root_txns;
  };
  EpochDag *current_epoch_dag;
  AtomicQueue<EpochDag *> epoch_dag_queue;
  std::atomic<uint> num_txns_left_in_epoch;
  pthread_cond_t epoch_finished_cond;
  pthread_mutex_t epoch_finished_mutex;
  void RunCalvinEpochScheduler();

  // 3) Executor
  // helper function to call calvin epoch executor in pthread
  static void *calvin_epoch_executor_helper(void *arg);

  void CalvinEpochExecutorFunc();
  void CalvinExecuteSingleEpoch(EpochDag* epoch_dag);

  // ===================== END OF CALVIN =======================

  // Serial validation
  bool SerialValidate(Txn *txn);

  // Parallel executtion/validation for OCC
  void ExecuteTxnParallel(Txn *txn);

  // Serial version of scheduler.
  void RunSerialScheduler();

  // Locking version of scheduler.
  void RunLockingScheduler();

  // OCC version of scheduler.
  void RunOCCScheduler();

  // OCC version of scheduler with parallel validation.
  void RunOCCParallelScheduler();

  // MVCC version of scheduler.
  void RunMVCCScheduler();

  // MVCC SSI version of scheduler.
  void RunMVCCSSIScheduler();

  // Performs all reads required to execute the transaction, then executes the
  // transaction logic.
  void ExecuteTxn(Txn *txn);

  // Applies all writes performed by '*txn' to 'storage_'.
  //
  // Requires: txn->Status() is COMPLETED_C.
  void ApplyWrites(Txn *txn);

  // The following functions are for MVCC.
  void MVCCExecuteTxn(Txn *txn);

  // The following functions are for MVCC_SSI.

  void MVCCSSIExecuteTxn(Txn *txn);

  void MVCCSSICheckReads(Txn *txn);

  // The following functions are for MVCC & MVCC_SSI.
  bool MVCCCheckWrites(Txn *txn);

  void MVCCLockWriteKeys(Txn *txn);

  void MVCCUnlockWriteKeys(Txn *txn);

  // Concurrency control mechanism the TxnProcessor is currently using.
  CCMode mode_;

  // Thread pool managing all threads used by TxnProcessor.
  StaticThreadPool tp_;

  // Data storage used for all modes.
  Storage *storage_;

  // Next valid unique_id, and a mutex to guard incoming txn requests.
  int next_unique_id_;
  Mutex mutex_;

  // Queue of incoming transaction requests.
  AtomicQueue<Txn *> txn_requests_;

  // Queue of txns that have acquired all locks and are ready to be executed.
  //
  // Does not need to be atomic because RunScheduler is the only thread that
  // will ever access this queue.
  deque<Txn *> ready_txns_;

  AtomicQueue<Txn *> calvin_ready_txns_;

  // Queue of completed (but not yet committed/aborted) transactions.
  AtomicQueue<Txn *> completed_txns_;

  // Vector of committed transactions that are used to check any overlap
  // during OCC validation phase.
  AtomicVector<Txn *> committed_txns_;

  // Queue of transaction results (already committed or aborted) to be returned
  // to client.
  AtomicQueue<Txn *> txn_results_;

  // Set of transactions that are currently in the process of parallel
  // validation.
  AtomicSet<Txn *> active_set_;

  // Used it for critical section in parallel occ.
  Mutex active_set_mutex_;

  // Lock Manager used for LOCKING concurrency implementations.
  LockManager *lm_;

  // Used for stopping the continuous loop that runs in the scheduler thread
  bool stopped_;

  // Gives us access to the scheduler thread so that we can wait for it to join
  // later.
  pthread_t scheduler_thread_;
};

#endif // _TXN_PROCESSOR_H_
