#!/bin/bash

sudo apt-get -y install make cmake gcc g++

# get worker number from hostname
WORKER=$(hostname | perl -lpe 's/.*[^\d](\d+)$/\1/g')

cd $HOME/onion
mkdir -p mybuild
cd mybuild
make clean
cmake ..
make
sudo make install

cd $HOME/fastcci
mkdir -p mybuild
cd mybuild
make clean
cmake ..
make
sudo make install

cd $HOME/fastcci
sudo cp assets/fastcci.service /lib/systemd/system/fastcci.service
cd /etc/systemd/system
sudo ln -sf /lib/systemd/system/fastcci.service .
sudo systemctl daemon-reload
sudo systemctl start fastcci.service

cd $HOME
(crontab -l ; echo "*/1 * * * * $HOME/bin/restart_fastcci.sh")| crontab -
