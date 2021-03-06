#!/bin/bash

# clean up the server in case of test failure
trap 'kill $(jobs -p)' EXIT

PORT=9998
FASTCCI_BIN=../build/fastcci
WS='curl -s -i -N -H "Connection: Upgrade" -H "Upgrade: websocket" -H "Host: localhost" -H "Sec-Websocket-Key: psst" -H "Sec-Websocket-Version: 13" http://localhost:'$PORT'/\?'
HTTP='curl -s -i -N http://localhost:'$PORT'/\?'
JS='curl -s -i -N http://localhost:'$PORT'/\?t=js\&'

# build the test database/
echo '== Building Database =='
$FASTCCI_BIN/fastcci_build_db < test_dump.txt || exit 1
[ $(md5sum 'done' | cut -c-8) = "d36f8f94" ] || exit 1
[ $(md5sum 'fastcci.cat' | cut -c-8) = "6ef81ddf" ] || exit 1
[ $(md5sum 'fastcci.tree' | cut -c-8) = "9c7acb41" ] || exit 1
echo 'passed.'
echo

# launch server (and wait for it to spin up)
export LD_LIBRARY_PATH=$HOME/lib:$LD_LIBRARY_PATH
$FASTCCI_BIN/fastcci_server $PORT . > /dev/null 2>&1 &
until $(curl -s  http://localhost:$PORT/status > /dev/null); do sleep 1; done

# test a few queries via websockets
echo '== Testing Websockets =='

eval "$WS"'c1=1\&d1=15\&s=200\&a=fqv' | grep -a 'RESULT 5,0,1|4,0,1|7,1,3|8,1,4' > /dev/null || exit 1
eval "$WS"'c1=1\&d1=15\&s=200\&a=fqv' | grep -a 'OUTOF 4' > /dev/null || exit 1

eval "$WS"'c1=1\&d1=0\&s=200\&a=fqv' | grep -a 'RESULT 5,0,1|4,0,1' > /dev/null || exit 1
eval "$WS"'c1=1\&d1=0\&s=200\&a=fqv' | grep -a 'OUTOF 2' > /dev/null || exit 1

eval "$WS"'c1=3\&d1=15\&s=200\&a=fqv' | grep -a 'RESULT 8,0,4' > /dev/null || exit 1
eval "$WS"'c1=3\&d1=15\&s=200\&a=fqv' | grep -a 'OUTOF 1' > /dev/null || exit 1

eval "$WS"'c1=100\&c2=200' | grep -a 'RESULT 101,0,0|104,2,0' > /dev/null || exit 1
eval "$WS"'c1=100\&c2=200' | grep -a 'OUTOF 2' > /dev/null || exit 1

eval "$WS"'c1=100\&c2=200\&a=not' | grep -a 'RESULT 102,0,0|103,1,0' > /dev/null || exit 1
eval "$WS"'c1=100\&c2=200\&a=not' | grep -a 'OUTOF 2' > /dev/null || exit 1
echo 'passed.'
echo

# test a few HTTP queries
echo '== Testing HTTP =='
eval "$HTTP"'c1=1\&d1=15\&s=200\&a=fqv' | grep '^RESULT 5,0,1|4,0,1|7,1,3|8,1,4$' > /dev/null || exit 1
echo 'passed.'
echo

# test a few JS callback queries
echo '== Testing JS Callback =='
eval "$JS"'c1=1\&d1=15\&s=200\&a=fqv' | grep "fastcciCallback( \[ 'RESULT 5,0,1|4,0,1|7,1,3|8,1,4', 'OUTOF 4'," > /dev/null || exit 1
echo 'passed.'
echo

killall fastcci_server
