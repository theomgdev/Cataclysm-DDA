#!/bin/sh

if (pwd | grep "Cataclysm-Signal/tools")
then
cd ..
else
if (ls Cataclysm-Signal)
then
echo "Cataclysm-Signal already exists"
else
git clone https://github.com/theomgdev/Cataclysm-Signal
fi
cd Cataclysm-Signal
fi

make

cd ..

if (ls dgamelaunch)
then
echo "dgamelaunch already exists"
else
git clone https://github.com/C0DEHERO/dgamelaunch
fi
cd dgamelaunch

./autogen.sh --enable-sqlite --enable-shmem --with-config-file=/opt/dgamelaunch/signal/etc/dgamelaunch.conf
make
sudo ./dgl-create-chroot
