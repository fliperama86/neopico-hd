import os
import re
from docling.document_converter import DocumentConverter

def convert_full_pdf(input_pdf, output_md):
    print(f"Starting full conversion of {input_pdf}...")
    converter = DocumentConverter()
    result = converter.convert(input_pdf)
    
    md_output = result.document.export_to_markdown()
    
    with open(output_md, "w", encoding="utf-8") as f:
        f.write(md_output)
    print(f"Saved full Markdown to {output_md}")
    return md_output

def split_markdown(full_md_path, output_dir):
    print(f"Splitting {full_md_path} into chapters...")
    if not os.path.exists(output_dir):
        os.makedirs(output_dir)
        
    with open(full_md_path, "r", encoding="utf-8") as f:
        content = f.read()
        
    # Split by major headers like "## 1. Introduction", "## 2. System bus"
    # Looking for "## [Number]. [Title]"
    chapters = re.split(r'\n(## \d+\. .*)\n', content)
    
    # The first element is the preamble (TOC, Colophon etc)
    preamble = chapters[0]
    with open(os.path.join(output_dir, "00_preamble.md"), "w", encoding="utf-8") as f:
        f.write(preamble)
        
    for i in range(1, len(chapters), 2):
        header = chapters[i]
        body = chapters[i+1] if i+1 < len(chapters) else ""
        
        # Clean header for filename
        # e.g., "## 1. Introduction" -> "01_introduction"
        match = re.search(r'## (\d+)\. (.*)', header)
        if match:
            num = match.group(1).zfill(2)
            title = match.group(2).lower().replace(" ", "_").replace("/", "_")
            # Remove non-alphanumeric chars from title
            title = re.sub(r'[^a-z0-9_]', '', title)
            filename = f"{num}_{title}.md"
            
            with open(os.path.join(output_dir, filename), "w", encoding="utf-8") as f:
                f.write(header + "\n" + body)
            print(f"Created {filename}")

if __name__ == "__main__":
    input_pdf = "docs/pico2-datasheet.pdf"
    base_dir = "docs/pico2-datasheet"
    full_md = os.path.join(base_dir, "pico2-datasheet-full.md")
    chapters_dir = base_dir # Storing directly in the folder
    
    # We can skip conversion if the full MD already exists for testing splitting
    if not os.path.exists(full_md):
        convert_full_pdf(input_pdf, full_md)
    
    split_markdown(full_md, chapters_dir)
    print("Full conversion and splitting complete!")

