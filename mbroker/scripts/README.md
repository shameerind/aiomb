# Q&A Extraction Scripts

Two scripts to extract questions and answers from JSON files.

## Scripts

### 1. extract_qa.py (Python)
Full-featured Python script with multiple output formats.

**Usage:**
```bash
# Basic usage (text output to stdout)
./extract_qa.py input.json

# Save to file
./extract_qa.py input.json -o output.txt

# Export as JSON
./extract_qa.py input.json -o output.json -f json

# Export as CSV
./extract_qa.py input.json -o output.csv -f csv
```

**Supported JSON structures:**
- Array of Q&A objects: `[{question, answer}, ...]`
- Object with questions array: `{questions: [{question, answer}, ...]}`
- Object with qa/items array: `{qa: [...]}` or `{items: [...]}`
- Flexible key names: `question/q/prompt` and `answer/a/response`

### 2. extract_qa.sh (Bash + jq)
Simple bash script using jq for quick extraction.

**Usage:**
```bash
# Output to stdout
./extract_qa.sh input.json

# Save to file
./extract_qa.sh input.json output.txt
```

**Requirements:** jq must be installed

## Example

Test with the provided example:
```bash
./extract_qa.py example.json
```

Output:
```
=== Q&A #1 ===
Question: What is mrepod?
Answer: mrepod is a daemon service that manages overlay filesystem mounts for sandboxes.

=== Q&A #2 ===
Question: How do I create a sandbox overlay?
Answer: Use the mrepo command: mrepo create <sandboxname>
...
```

## Supported Input Formats

**Format 1: Array**
```json
[
  {"question": "Q1?", "answer": "A1"},
  {"question": "Q2?", "answer": "A2"}
]
```

**Format 2: Object with questions**
```json
{
  "questions": [
    {"question": "Q1?", "answer": "A1"}
  ]
}
```

**Format 3: Short keys**
```json
[
  {"q": "Q1?", "a": "A1"}
]
```
