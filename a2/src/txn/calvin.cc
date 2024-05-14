#include "txn_processor.h"
#include "utils/common.h"
#include <chrono>
#include <set>
#include <stdio.h>
#include <unordered_set>

#include "lock_manager.h"

// Calvin Continuous Execution -- Global Locks
void TxnProcessor::CalvinContStartWorkers() {

}

void TxnProcessor::RunCalvinContScheduler() {

}

void TxnProcessor::CalvinContExecutorFunc() {

}

// Calvin Continuous Execution -- Individual Locks
void TxnProcessor::CalvinContIndivStartWorkers() {

}

void TxnProcessor::RunCalvinContIndivScheduler() {

}

void TxnProcessor::CalvinContIndivExecutorFunc() {

}

// Calvin Epoch Execution
void *TxnProcessor::calvin_sequencer_helper(void *arg) {
  reinterpret_cast<TxnProcessor *>(arg)->RunCalvinEpochSequencer();
  return NULL;
}
void TxnProcessor::RunCalvinEpochSequencer() {
    
}

void TxnProcessor::CalvinEpochStartWorkers() {

}

void TxnProcessor::RunCalvinEpochScheduler() {

}

void TxnProcessor::CalvinEpochExecutorFunc() {

}