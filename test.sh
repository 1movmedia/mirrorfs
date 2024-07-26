#!/bin/bash

which fusermount3 > /dev/null

# setup
mkdir -p mnt a b
trap 'rm -rf mnt a b' EXIT

set -ex

./mirrorfs a b mnt
trap 'fusermount3 -q -u mnt; rm -rf mnt a b' EXIT

# test read and write
echo foo > mnt/foo
test $(cat a/foo) == foo
test $(cat b/foo) == foo

# test rename
mv mnt/foo mnt/bar
test $(cat a/bar) == foo
test $(cat b/bar) == foo

# test metadata
chmod +x mnt/bar
test -x a/bar
test -x b/bar
