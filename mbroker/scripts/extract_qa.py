#!/usr/bin/env python3
"""
Extract questions and answers from JSON file
"""

import json
import sys
import argparse

def extract_qa(json_file, output_file=None):
    """Extract questions and answers from JSON file"""
    try:
        with open(json_file, 'r') as f:
            data = json.load(f)
    except FileNotFoundError:
        print(f"Error: File '{json_file}' not found", file=sys.stderr)
        return 1
    except json.JSONDecodeError as e:
        print(f"Error: Invalid JSON in '{json_file}': {e}", file=sys.stderr)
        return 1
    
    output = sys.stdout if output_file is None else open(output_file, 'w')
    
    try:
        # Handle different JSON structures
        items = []
        
        # Check if it's a list
        if isinstance(data, list):
            items = data
        # Check if it's a dict with common keys
        elif isinstance(data, dict):
            # Special handling for nested 'rows' structure (like gsm8 dataset)
            if 'rows' in data:
                # Extract the 'row' object from each item in 'rows'
                items = [item['row'] for item in data['rows'] if 'row' in item]
            elif 'questions' in data:
                items = data['questions']
            elif 'qa' in data:
                items = data['qa']
            elif 'items' in data:
                items = data['items']
            else:
                # Treat the whole dict as a single item
                items = [data]
        
        # Extract Q&A pairs
        count = 0
        for i, item in enumerate(items, 1):
            if isinstance(item, dict):
                # Try different key names for question
                question = (item.get('question') or 
                           item.get('q') or 
                           item.get('prompt') or 
                           item.get('text') or
                           'N/A')
                
                # Try different key names for answer
                answer = (item.get('answer') or 
                         item.get('a') or 
                         item.get('response') or 
                         item.get('reply') or
                         'N/A')
                
                output.write(f"=== Q&A #{i} ===\n")
                output.write(f"Question: {question}\n")
                output.write(f"Answer: {answer}\n")
                output.write("\n")
                count += 1
            else:
                output.write(f"=== Item #{i} ===\n")
                output.write(f"{item}\n")
                output.write("\n")
                count += 1
        
        print(f"Extracted {count} items", file=sys.stderr)
        return 0
        
    finally:
        if output_file:
            output.close()

def main():
    parser = argparse.ArgumentParser(description='Extract questions and answers from JSON file')
    parser.add_argument('json_file', help='Input JSON file')
    parser.add_argument('-o', '--output', help='Output file (default: stdout)')
    parser.add_argument('-f', '--format', choices=['text', 'json', 'csv'], 
                       default='text', help='Output format (default: text)')
    
    args = parser.parse_args()
    
    if args.format == 'text':
        return extract_qa(args.json_file, args.output)
    elif args.format == 'json':
        return extract_qa_json(args.json_file, args.output)
    elif args.format == 'csv':
        return extract_qa_csv(args.json_file, args.output)

def extract_qa_json(json_file, output_file=None):
    """Extract Q&A in JSON format"""
    try:
        with open(json_file, 'r') as f:
            data = json.load(f)
    except (FileNotFoundError, json.JSONDecodeError) as e:
        print(f"Error: {e}", file=sys.stderr)
        return 1
    
    items = []
    if isinstance(data, list):
        items = data
    elif isinstance(data, dict):
        # Special handling for nested 'rows' structure
        if 'rows' in data:
            items = [item['row'] for item in data['rows'] if 'row' in item]
        else:
            items = data.get('questions') or data.get('qa') or data.get('items') or [data]
    
    qa_pairs = []
    for item in items:
        if isinstance(item, dict):
            qa_pairs.append({
                'question': (item.get('question') or item.get('q') or item.get('prompt') or 'N/A'),
                'answer': (item.get('answer') or item.get('a') or item.get('response') or 'N/A')
            })
    
    output = sys.stdout if output_file is None else open(output_file, 'w')
    json.dump(qa_pairs, output, indent=2)
    if output_file:
        output.close()
    
    return 0

def extract_qa_csv(json_file, output_file=None):
    """Extract Q&A in CSV format"""
    import csv
    
    try:
        with open(json_file, 'r') as f:
            data = json.load(f)
    except (FileNotFoundError, json.JSONDecodeError) as e:
        print(f"Error: {e}", file=sys.stderr)
        return 1
    
    items = []
    if isinstance(data, list):
        items = data
    elif isinstance(data, dict):
        # Special handling for nested 'rows' structure
        if 'rows' in data:
            items = [item['row'] for item in data['rows'] if 'row' in item]
        else:
            items = data.get('questions') or data.get('qa') or data.get('items') or [data]
    
    output = sys.stdout if output_file is None else open(output_file, 'w', newline='')
    writer = csv.writer(output)
    writer.writerow(['Question', 'Answer'])
    
    for item in items:
        if isinstance(item, dict):
            question = (item.get('question') or item.get('q') or item.get('prompt') or 'N/A')
            answer = (item.get('answer') or item.get('a') or item.get('response') or 'N/A')
            writer.writerow([question, answer])
    
    if output_file:
        output.close()
    
    return 0

if __name__ == '__main__':
    sys.exit(main())
