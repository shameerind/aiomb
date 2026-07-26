#!/bin/bash
# Extract questions and answers from JSON file using jq

if [ $# -eq 0 ]; then
    echo "Usage: $0 <json_file> [output_file]"
    echo ""
    echo "Examples:"
    echo "  $0 data.json"
    echo "  $0 data.json output.txt"
    exit 1
fi

JSON_FILE="$1"
OUTPUT_FILE="${2:-/dev/stdout}"

if [ ! -f "$JSON_FILE" ]; then
    echo "Error: File '$JSON_FILE' not found" >&2
    exit 1
fi

# Check if jq is installed
if ! command -v jq &> /dev/null; then
    echo "Error: jq is not installed. Please install jq or use extract_qa.py" >&2
    exit 1
fi

{
    echo "Extracting Q&A from: $JSON_FILE"
    echo "================================"
    echo ""
    
    # Try to extract Q&A pairs from different JSON structures
    # Handle array of objects with question/answer keys
    jq -r 'if type == "array" then
             .[] | select(type == "object")
           elif type == "object" then
             if .questions then .questions[]
             elif .qa then .qa[]
             elif .items then .items[]
             else .
             end
           else .
           end |
           if type == "object" then
             "=== Q&A ===\n" +
             "Question: " + (.question // .q // .prompt // "N/A") + "\n" +
             "Answer: " + (.answer // .a // .response // "N/A") + "\n"
           else
             "Item: " + (. | tostring) + "\n"
           end' "$JSON_FILE" 2>/dev/null || {
        echo "Error: Failed to parse JSON" >&2
        exit 1
    }
} > "$OUTPUT_FILE"

echo "Done. Output written to: $OUTPUT_FILE" >&2
