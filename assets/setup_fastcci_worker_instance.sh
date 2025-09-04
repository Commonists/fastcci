#!/bin/bash

# install prerequisites
sudo apt-get -y install rsync make cmake gcc g++ libgnutls28-dev libgcrypt20-dev zlib1g-dev zlib1g

# build onion http and websocket library
cd $HOME/fastcci
mkdir -p build/onion
cd build/onion
make clean
cmake ../../onion
make
sudo make install
sudo ldconfig

# build the fastcci server and database builder
cd $HOME/fastcci
mkdir -p build/fastcci
cd build/fastcci
make clean
cmake ../..
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

# make sure master server can push updated DBs
(crontab -l ; echo "*/1 * * * * sudo cp $HOME/.ssh/id_rsa.pub /etc/ssh/userkeys/$USER")| crontab -
