#!/bin/bash

which fusermount3 > /dev/null

# setup
mkdir -p mnt a b c
trap 'rm -rf mnt a b c' EXIT

set -ex

./mirrorfs a b c mnt
trap 'fusermount3 -q -u mnt; rm -rf mnt a b c' EXIT

# test read and write
echo foo > mnt/foo
test $(cat a/foo) == foo
test $(cat b/foo) == foo
test $(cat c/foo) == foo

# test rename
mv mnt/foo mnt/bar
test $(cat a/bar) == foo
test $(cat b/bar) == foo
test $(cat c/bar) == foo

# test metadata
chmod +x mnt/bar
test -x a/bar
test -x b/bar
test -x c/bar
