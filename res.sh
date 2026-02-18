#!/bin/bash
sort -k3,3nr -k2,2nr score.txt | head -n1 | awk '{print $1}'