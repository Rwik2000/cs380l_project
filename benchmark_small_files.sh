#!/bin/bash

# Define parameters
NUM_FILES=100
FILE_SIZE=10M
SRC_DIR="small_files"
RESULTS_FILE="results_small_files_raw.csv"
TRIALS=10

# Cleanup and setup
rm -rf $SRC_DIR small_files_cp small_files_mv small_files_rsync small_files_custom small_files_io_uring_cp small_files_io_uring_mv
mkdir $SRC_DIR

# Generate small files
echo "Generating $NUM_FILES files of size $FILE_SIZE in $SRC_DIR..."
for i in $(seq 1 $NUM_FILES); do
    dd if=/dev/zero of=$SRC_DIR/file_$i bs=$FILE_SIZE count=1 >/dev/null 2>&1
done

# Prepare results file
echo "Tool,Operation,Trial,Time(s),User Time(s),System Time(s),Max Memory(KB)" > $RESULTS_FILE

# Benchmark function
benchmark() {
    TOOL=$1
    CMD=$2
    OPERATION=$3
    DEST_DIR=$4




    for TRIAL in $(seq 1 $TRIALS); do


        TMP_SRC_DIR="small_files_tmp"
        rm -rf $TMP_SRC_DIR
        cp -r $SRC_DIR $TMP_SRC_DIR

        echo "Trial $TRIAL: Benchmarking $TOOL for $OPERATION..."
        
        # Flush disk caches before running the command
        sync && echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null

        # Create a unique destination directory for each trial
        # TRIAL_DEST_DIR="${DEST_DIR}"
        mkdir $DEST_DIR

        # Run the command
        /usr/bin/time -v $CMD $TMP_SRC_DIR $DEST_DIR 2>tmp_time.log
        sync  # Flush caches again after the operation

        # Extract timing and memory information
        TIME=$(grep "Elapsed (wall clock) time" tmp_time.log | awk '{print $8}')
        USER_TIME=$(grep "User time (seconds)" tmp_time.log | awk '{print $4}')
        SYSTEM_TIME=$(grep "System time (seconds)" tmp_time.log | awk '{print $4}')
        MAX_MEM=$(grep "Maximum resident set size" tmp_time.log | awk '{print $6}')
        
        echo "$TOOL,$OPERATION,$TRIAL,$TIME,$USER_TIME,$SYSTEM_TIME,$MAX_MEM" >> $RESULTS_FILE

        # Cleanup: Remove destination directory after last trial, if needed
        if [ "$TRIAL" -eq "$TRIALS" ]; then
            echo "Preserving final result for $TOOL in $TRIAL_DEST_DIR"
        else
            rm -rf $DEST_DIR
        fi
    done
}

# Run benchmarks
benchmark "cp" "cp -r" "Copy" "small_files_cp"
benchmark "io_uring cp" "./compiled/io_uring_copy_dir" "Copy io_uring" "small_files_cp_io_uring"

benchmark "mv" "mv -r" "Move" "small_files_mv"
benchmark "io_uring mv" "./compiled/io_uring_mv_dir" "Move io_uring" "small_files_mv_io_uring"

benchmark "rsync" "rsync -r" "rsync" "small_files_rsync"
benchmark "io_uring rsync" "./compiled/io_uring_rsync" "rsync io_uring" "small_files_rsync_io_uring"

# Cleanup temporary files
rm -rf tmp_time.log

echo "Benchmarking complete. Raw results saved to $RESULTS_FILE."
