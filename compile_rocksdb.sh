sudo rm -rf /home/afschy/db_extra/*
echo deadline | sudo tee -a /sys/class/block/nvme0n1/queue/scheduler
sudo make clean
sudo DEBUG_LEVEL=0 USE_RTTI=1 ROCKSDB_PLUGINS=zenfs make -j60 static_lib install
cd plugin/zenfs/util
PKG_CONFIG_PATH=/usr/local/lib/pkgconfig make clean
PKG_CONFIG_PATH=/usr/local/lib/pkgconfig make
sudo ./zenfs mkfs --force --zbd=nvme0n1 --aux_path=/home/afschy/db_extra/
