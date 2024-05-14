#!/bin/bash
#SBATCH -N 1
#SBATCH --exclusive
#SBATCH -c 12
#SBATCH -t 10
#SBATCH -A bhatele-lab-cmsc


module load llvm/11.1.0
cd /scratch/zt1/project/bhatele-lab/user/amovsesy/CMSC624/cmsc624-final-project/a2/

./build/src/txn_processor_test > benchmark_results.txt
 
