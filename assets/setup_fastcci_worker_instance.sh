#!/bin/bash

# install prerequisites
sudo apt-get -y install make cmake gcc g++

# build onion http and websocket library
cd $HOME/onion
mkdir -p mybuild
cd mybuild
make clean
cmake ..
make
sudo make install

# build the fastcci server and database builder
cd $HOME/fastcci
mkdir -p mybuild
cd mybuild
make clean
cmake ..
make
sudo make install

# register fastcci server with systemd
cd $HOME/fastcci
sudo cp assets/fastcci.service /lib/systemd/system/fastcci.service
cd /etc/systemd/system
sudo ln -sf /lib/systemd/system/fastcci.service .
sudo systemctl daemon-reload
sudo systemctl start fastcci.service

# add cron entry for database update
cd $HOME
(crontab -l ; echo "*/1 * * * * $HOME/bin/restart_fastcci.sh")| crontab -

# make sure master derver can push updated DBs
sudo cp $HOME/.ssh/id_rsa.pub /etc/ssh/userkeys/dschwen
