import os
from pypdf import PdfReader, PdfWriter
from docling.datamodel.base_models import InputFormat
from docling.document_converter import DocumentConverter

def extract_pages(input_pdf, output_pdf, start_page, end_page):
    print(f"Extracting pages {start_page} to {end_page}...")
    reader = PdfReader(input_pdf)
    writer = PdfWriter()
    
    # end_page is exclusive in range, but inclusive in our logic
    for i in range(start_page, min(end_page + 1, len(reader.pages))):
        writer.add_page(reader.pages[i])
        
    with open(output_pdf, "wb") as f:
        writer.write(f)
    print(f"Saved extracted pages to {output_pdf}")

def convert_to_md(input_pdf, output_md):
    print(f"Converting {input_pdf} to Markdown using Docling...")
    converter = DocumentConverter()
    result = converter.convert(input_pdf)
    
    md_output = result.document.export_to_markdown()
    
    with open(output_md, "w", encoding="utf-8") as f:
        f.write(md_output)
    print(f"Saved Markdown to {output_md}")

if __name__ == "__main__":
    base_dir = "docs/pico2-datasheet"
    input_pdf = "docs/pico2-datasheet.pdf"
    temp_pdf = os.path.join(base_dir, "pilot_pages_1_50.pdf")
    pilot_md = os.path.join(base_dir, "pilot_pages_1_50.md")
    
    # Step 1: Extract pages
    extract_pages(input_pdf, temp_pdf, 0, 49) # 0-indexed, first 50 pages
    
    # Step 2: Convert
    convert_to_md(temp_pdf, pilot_md)
    
    print("Pilot conversion complete!")

