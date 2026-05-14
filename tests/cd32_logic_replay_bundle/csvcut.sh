#!/bin/bash

# Check if a filename was provided
if [ -z "$1" ]; then
    echo "Usage: $0 <filename.csv>"
    exit 1
fi

FILE=$1
# Define the size to keep (100MB)
KEEP_SIZE_MB=100
# Convert MB to bytes
BYTES_TO_KEEP=$((KEEP_SIZE_MB * 1024 * 1024))

OUTPUT_FILE="sample_$FILE"

echo "Extracting the first ${KEEP_SIZE_MB}MB from $FILE..."

# 1. Grab the first 100MB of bytes
# 2. Use 'sed' to delete the very last (likely partial) line
head -c "$BYTES_TO_KEEP" "$FILE" | sed '$d' > "$OUTPUT_FILE"

echo "Done. Created $OUTPUT_FILE"
echo "Final size: $(du -h "$OUTPUT_FILE" | cut -f1)"
