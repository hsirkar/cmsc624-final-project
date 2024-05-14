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
void TxnProcessor::RunCalvinContScheduler() {

}

void TxnProcessor::CalvinContExecutorFunc() {
  std::cout << "Calvin Continuous Executor Function" << std::endl;
}

/***********************************************
*  Calvin Continuous Execution -- Indiv Locks  *
***********************************************/
void TxnProcessor::RunCalvinContIndivScheduler() {
  
}

void TxnProcessor::CalvinContIndivExecutorFunc() {
  std::cout << "Calvin Continuous Individual Executor Function" << std::endl;
}

/***********************************************
*            Calvin Epoch Execution            *
***********************************************/
void *TxnProcessor::calvin_sequencer_helper(void *arg) {
  reinterpret_cast<TxnProcessor *>(arg)->RunCalvinEpochSequencer();
  return NULL;
}
void TxnProcessor::RunCalvinEpochSequencer() {
    
}

void TxnProcessor::RunCalvinEpochScheduler() {

}

void TxnProcessor::CalvinEpochExecutorFunc() {
  std::cout << "Calvin Epoch Executor Function" << std::endl;
}