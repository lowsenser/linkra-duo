#!/usr/bin/env python3
"""
Build Web Interface und gzipe für ESP32
"""

import os
import subprocess
import gzip

def build_web():
    """Vite Build durchführen"""
    web_dir = os.path.dirname(os.path.abspath(__file__)) + "/../web"
    
    print("📦 Building web interface...")
    os.chdir(web_dir)
    
    # npm dependencies installieren (falls nicht vorhanden)
    if not os.path.exists("node_modules"):
        print("📥 Installing dependencies...")
        subprocess.run(["npm", "install"], check=True)
    
    # Build durchführen
    subprocess.run(["npm", "run", "build"], check=True)
    
    print("✅ Web build complete")

def gzip_files():
    """Output-Dateien gzipen"""
    data_dir = os.path.dirname(os.path.abspath(__file__)) + "/../data"
    
    print("📦 Compressing files...")
    
    if not os.path.exists(data_dir):
        print("❌ No data directory found")
        return
    
    for root, dirs, files in os.walk(data_dir):
        for file in files:
            if file.endswith('.gz'):
                continue
                
            file_path = os.path.join(root, file)
            gz_path = file_path + '.gz'
            
            with open(file_path, 'rb') as f_in:
                with gzip.open(gz_path, 'wb') as f_out:
                    f_out.write(f_in.read())
            
            # Original-Größe vs komprimierte Größe anzeigen
            orig_size = os.path.getsize(file_path)
            gz_size = os.path.getsize(gz_path)
            ratio = (1 - gz_size / orig_size) * 100
            
            print(f"  {file}: {orig_size} → {gz_size} bytes ({ratio:.1f}% saved)")

if __name__ == "__main__":
    build_web()
    gzip_files()
    print("✨ Build complete!")
