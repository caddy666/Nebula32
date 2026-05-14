import os
from PIL import Image

def resize_images(input_folder, output_folder, size=(300, 300), quality=85):
    # Create output folder if it doesn't exist
    if not os.path.exists(output_folder):
        os.makedirs(output_folder)
        print(f"Created directory: {output_folder}")

    # Loop through files in the input folder
    for filename in os.listdir(input_folder):
        if filename.lower().endswith((".jpg", ".jpeg")):
            try:
                img_path = os.path.join(input_folder, filename)
                with Image.open(img_path) as img:
                    # Convert to RGB if necessary (e.g., if it's a PNG renamed to JPG)
                    if img.mode != 'RGB':
                        img = img.convert('RGB')
                    
                    # Resize using LANCZOS for high-quality downsampling
                    img_resized = img.resize(size, Image.Resampling.LANCZOS)
                    
                    # Define output path
                    output_path = os.path.join(output_folder, filename)
                    
                    # Save with specified quality
                    img_resized.save(output_path, "JPEG", quality=quality)
                    print(f"Processed: {filename}")
            except Exception as e:
                print(f"Failed to process {filename}: {e}")

# --- CONFIGURATION ---
INPUT_DIR = "raw_covers"    # Your source folder
OUTPUT_DIR = "resized_covers" # Where the 300x300 images will go

resize_images(INPUT_DIR, OUTPUT_DIR)
print("Batch processing complete.")