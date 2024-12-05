#!/bin/bash

# Remove previous results
RESULTS_FILE="benchmark_results.csv"
rm -f $RESULTS_FILE

# Create the results file and add a header
echo "File Size,Tool,Run,Time(s),User Time(s),System Time(s),Max Memory(KB)" > $RESULTS_FILE

# File sizes to test
FILE_SIZES=("10K" "100K" "1M" "10M" "100M")
SAMPLES=5

# Directories
SRC_DIR="testingdir"
DST_DIR="testingdircopy"
COMMANDS=("./compiled/io_uring_recursive" "cp -r")

# Function to generate files
generate_files() {
    local file_size=$1
    local num_files=16

    rm -rf $SRC_DIR
    mkdir $SRC_DIR
    echo "Generating $num_files files of size $file_size in $SRC_DIR..."

    for i in $(seq 1 $num_files); do
        dd if=/dev/urandom bs=$file_size count=1 of=$SRC_DIR/test-$i.bin status=none
    done
}

benchmark() {
    local tool=$1
    local command=$2
    local file_size=$3

    for run in $(seq 1 $SAMPLES); do
        # Flush disk caches
        sync
        sudo sh -c 'echo 3 > /proc/sys/vm/drop_caches'

        # Record start time
        local start_time=$(date +%s%N)

        # Run the command
        sh -c "$command $SRC_DIR $DST_DIR"

        # Record end time
        local end_time=$(date +%s%N)

        # Calculate elapsed time in seconds with high precision
        local elapsed_ns=$((end_time - start_time))
        local elapsed_sec=$(echo "scale=9; $elapsed_ns / 1000000000" | bc)

        # Extract user/system times and memory usage
        /usr/bin/time -v sh -c "$command $SRC_DIR $DST_DIR" 2>tmp_time.log

        USER_TIME=$(grep "User time (seconds)" tmp_time.log | awk '{print $4}')
        SYSTEM_TIME=$(grep "System time (seconds)" tmp_time.log | awk '{print $4}')
        MAX_MEM=$(grep "Maximum resident set size" tmp_time.log | awk '{print $6}')

        # Log results with high precision time
        echo "$file_size,$tool,$run,$elapsed_sec,$USER_TIME,$SYSTEM_TIME,$MAX_MEM" >> $RESULTS_FILE

        # Cleanup destination directory
        rm -rf $DST_DIR
    done
}


# Main loop to run benchmarks for different file sizes
for size in "${FILE_SIZES[@]}"; do
    generate_files $size

    for cmd in "${COMMANDS[@]}"; do
        if [[ $cmd == "./compiled/io_uring_recursive" ]]; then
            TOOL="io_uring_recursive"
        else
            TOOL="cp -r"
        fi

        benchmark "$TOOL" "$cmd" "$size"
    done
done

# Cleanup
rm -rf $SRC_DIR $DST_DIR tmp_time.log
echo "Benchmarking complete. Results saved to $RESULTS_FILE."
