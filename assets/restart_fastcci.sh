#!/bin/bash

# is the transfer done
DONE="/tmp/done"
if [ -e $DONE ] 
then
  # delete update trigger
  rm $DONE

  #sudo restart fastcci-server
  sudo systemctl stop fastcci.service 
  sudo systemctl start fastcci.service 
fi
