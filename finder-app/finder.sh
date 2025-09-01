#!/bin/bash


filesdir="$1"
searchstr="$2"

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


if [ ! -d "$filesdir" ]
then
	echo "Error: $filesdir is not a directory"
	exit 1
fi

file=$(find "$filesdir" -type f | wc -l)

found=$(grep -r "$searchstr" "$filesdir" | wc -l)

echo "The number of files are $file and the number of matching lines are $found"
