#!/bin/bash

FASTCCI_BIN=../build/fastcci
WS='curl -s -i -N -H "Connection: Upgrade" -H "Upgrade: websocket" -H "Host: localhost" -H "Sec-Websocket-Key: psst" -H "Sec-Websocket-Version: 1" http://localhost:8080/\?'
HTTP='curl -s -i -N http://localhost:8080/\?'
JS='curl -s -i -N http://localhost:8080/\?t=js\&'

# build the test database
echo '== Building Database =='
$FASTCCI_BIN/fastcci_build_db < test_dump.txt || exit 1
[ $(md5sum 'done' | cut -c-8) = "d36f8f94" ] || exit 1
[ $(md5sum 'fastcci.cat' | cut -c-8) = "6ef81ddf" ] || exit 1
[ $(md5sum 'fastcci.tree' | cut -c-8) = "9c7acb41" ] || exit 1
echo 'passed.'
echo

# launch server (and wait for it to spin up)
export LD_LIBRARY_PATH=$HOME/lib:$LD_LIBRARY_PATH
$FASTCCI_BIN/fastcci_server 8080 . > /dev/null 2>&1 &
until $(curl -s  http://localhost:8080/status > /dev/null); do sleep 1; done

# test a few queries via websockets
echo '== Testing Websockets =='
[ "$(eval $WS'c1=1\&d1=15\&s=200\&a=fqv' | grep RESULT | cut -d' ' -f2)" = "5,0,1|4,0,1|7,1,2|8,1,3" ]  || exit 1
[ "$(eval $WS'c1=1\&d1=0\&s=200\&a=fqv' | grep RESULT | cut -d' ' -f2)" = "5,0,1|4,0,1" ]  || exit 1
[ "$(eval $WS'c1=3\&d1=15\&s=200\&a=fqv' | grep RESULT | cut -d' ' -f2)" = "8,0,3" ]  || exit1
[ "$(eval $WS'c1=100\&c2=200' | grep RESULT | cut -d' ' -f2)" = "101,0,0|104,2,0" ]  || exit1
[ "$(eval $WS'c1=100\&c2=200\&a=not' | grep RESULT | cut -d' ' -f2)" = "102,0,0|103,1,0" ]  || exit1
echo 'passed.'
echo

# test a few HTTP queries
echo '== Testing HTTP =='
[ "$(eval $HTTP'c1=1\&d1=15\&s=200\&a=fqv' | grep RESULT | cut -d' ' -f2)" = "5,0,1|4,0,1|7,1,2|8,1,3" ]  || exit 1
echo 'passed.'
echo

# test a few JS callback queries
echo '== Testing JS Callback =='
[ "$(eval $JS'c1=1\&d1=15\&s=200\&a=fqv' | grep fastcciCallback | cut -c -63)" = "fastcciCallback( [ 'RESULT 5,0,1|4,0,1|7,1,2|8,1,3', 'OUTOF 4'," ]  || exit 1
echo 'passed.'
echo

killall fastcci_server
