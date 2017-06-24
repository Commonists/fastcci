#!/bin/bash

# is the transfer done
DONE="$HOME/data/done2"
echo $DONE
if [ ! -e $DONE ] 
then
  exit
fi

# check if we need to restart
STAMP=$HOME/data/$(hostname)
echo $STAMP
if [ -e $STAMP ] && [ $STAMP -nt $DONE ]
then
  exit
fi

#sudo restart fastcci-server
sudo systemctl stop fastcci.service 
sudo systemctl start fastcci.service 

# mark current server as restarted
touch $STAMP
