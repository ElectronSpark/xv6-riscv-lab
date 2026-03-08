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
