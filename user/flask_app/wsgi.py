"""
WSGI entry point — run the Flask app under Waitress (production server).

Usage:
    python3 /app/wsgi.py
"""
import sys
import os

# Ensure /app is on the Python path so imports resolve
sys.path.insert(0, '/app')
os.chdir('/app')

from app import create_app

application = create_app()

if __name__ == '__main__':
    # xv6 does not support userspace threads, so we use Flask's built-in
    # single-threaded server instead of Waitress.
    print('xv6 web server: starting on 0.0.0.0:80 ...', flush=True)
    application.run(host='0.0.0.0', port=80,
                    debug=False, use_reloader=False, threaded=False)
