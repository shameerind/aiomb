#!/bin/bash

cd /b/workspace
mkdir -p ot/{lower,upper,work} mysb
touch ot/lower/{shm,shm1,shm2,shm3};
echo "testing file contents"  >> ot/lower/shm
sudo mount -t overlay overlay  -o lowerdir=ot/lower,upperdir=ot/upper,workdir=ot/work,nfs_export=on,index=on,override_creds=18201:950 mysb

echo result: $?