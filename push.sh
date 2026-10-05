#!/bin/bash

# Check if a commit message was provided
if [ -z "$1" ]; then
    echo "Error: No commit message provided."
    echo "Usage: ./push.sh \"Your commit message\""
    exit 1
fi

echo "Adding changes..."
git add .

echo "Committing with message: $1"
git commit -m "$1"

echo "Pushing to GitHub..."
git push

echo "Done!"
