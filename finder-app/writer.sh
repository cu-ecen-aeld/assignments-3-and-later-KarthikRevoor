#!/bin/bash

writefile=$1
writestr=$2


if [ $# -lt 2 ]
then
	echo "Error: Must provide 2 arguments <directory> <search_string>"
	exit 1
else
	if [ $# -gt 2 ]
	then
		echo "Error: Many Arguements provided <directory> <search_string>"
		exit 1
	fi
fi

mkdir -p "$(dirname "$writefile")"
echo "$writestr" > "$writefile"

if [ $? -ne 0 ]
then
	echo "Error: Cannot create the file $writefile"
	exit 1
fi

echo "File is created at $writefile which has $writestr"
