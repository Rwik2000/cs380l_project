#!/bin/bash
rm -rf testingdir
rm -rf testingdircopy

mkdir testingdir
cd testingdir

echo "Creating 16 files of size 256MB" 
for i in {1..16}
do
   dd if=/dev/urandom bs=1024 count=262144 of=test-${i}.bin status=none
done

cd ..
echo "Testing with ./cpr"
./test.sh ./compiled/io_uring_recursive testingdir testingdircopy

echo "Testing with cp -r" 
rm -rf testingdircopy
./test.sh "cp -r" testingdir testingdircopy


echo "16 files 256 MB tests done"

rm -rf testingdir
rm -rf testingdircopy

