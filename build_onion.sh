#!/bin/bash

cd onion
mkdir mybuild
cd mybuild
cmake ..
make
sudo make install

