#!/bin/bash

sudo apt-get -y install make cmake gcc g++

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
sudo ln -s /lib/systemd/system/fastcci.service .
sudo systemctl daemon-reload
sudo systemctl start fastcci.service

