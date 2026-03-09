"""
xv6 Personal Web Server — Flask Application Factory
====================================================
A full-stack web application running on xv6.

Start with:
    python3 /app/wsgi.py

Or for development:
    python3 /app/app.py
"""
import os
import secrets


def create_app():
    """Application factory — creates and configures the Flask app."""
    from flask import Flask

    app = Flask(__name__,
                template_folder='/app/templates',
                static_folder='/app/static')

    # Configuration
    app.config['SECRET_KEY'] = os.environ.get('SECRET_KEY',
                                               secrets.token_hex(32))
    app.config['SQLALCHEMY_DATABASE_URI'] = 'sqlite:////var/lib/app.db'
    app.config['SQLALCHEMY_TRACK_MODIFICATIONS'] = False
    app.config['SESSION_TYPE'] = 'filesystem'
    app.config['SESSION_FILE_DIR'] = '/tmp/flask_sessions'
    app.config['MAX_CONTENT_LENGTH'] = 16 * 1024 * 1024  # 16 MB upload limit

    # Initialize extensions
    from models import db, login_manager
    db.init_app(app)
    login_manager.init_app(app)

    # ── SQLite performance tuning ──────────────────────────────────────────
    # All current xv6 platforms mount the root filesystem from a ramdisk
    # (Orange Pi via U-Boot initrd, x86 via QEMU -initrd).  The ramdisk is
    # volatile memory backed by memmove(); ramdisk_flush() is a no-op.
    # Crash-safety pragmas therefore only add CPU overhead for ext4 metadata
    # churn (journal-file create → write → fsync → unlink → inode destroy)
    # without any durability benefit.
    #
    # journal_mode=OFF    eliminates the rollback journal entirely — no file
    #   create/unlink per commit, no htree dir-entry add/remove, no inode
    #   alloc/free.  Cuts ~8 ext4 cache flushes per COMMIT to ~1.
    # synchronous=OFF     skips fdatasync() calls on the DB file — they
    #   resolve to ext4_block_cache_flush + ramdisk_flush (a no-op), but
    #   still take CPU time iterating the dirty list.
    from sqlalchemy import event
    from sqlalchemy.engine import Engine
    import sqlite3 as _sqlite3

    @event.listens_for(Engine, "connect")
    def _set_sqlite_pragma(dbapi_connection, connection_record):
        if isinstance(dbapi_connection, _sqlite3.Connection):
            cursor = dbapi_connection.cursor()
            cursor.execute("PRAGMA synchronous = OFF")
            cursor.execute("PRAGMA journal_mode = OFF")
            cursor.close()

    # Register blueprints
    from auth import auth_bp
    from main import main_bp
    app.register_blueprint(auth_bp)
    app.register_blueprint(main_bp)

    # Create database tables on first request
    with app.app_context():
        os.makedirs('/var/lib', exist_ok=True)
        db.create_all()

    return app


if __name__ == '__main__':
    application = create_app()
    # Bind to 0.0.0.0:80 — QEMU forwards host:8080 -> guest:80
    # Disable reloader (no inotify on xv6)
    application.run(host='0.0.0.0', port=80,
                    debug=True, use_reloader=False)
