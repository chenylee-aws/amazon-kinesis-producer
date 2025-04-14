#!/bin/bash


EXECUTABLE="/local/tmp/kinesis_producer"

valgrind --tool=memcheck \
         --leak-check=full \
         --show-leak-kinds=all \
         --track-origins=yes \
         --verbose \
         --log-file=valgrind_output.log \
         "$EXECUTABLE" "$@"