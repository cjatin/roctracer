#!/bin/bash

fatal() {
  echo "$1"
  exit 1
}

if [ -z "$BUILD_DIR" ] ; then fatal "env var BUILD_DIR is not defined"; fi

cd $BUILD_DIR
./run.sh

exit 0