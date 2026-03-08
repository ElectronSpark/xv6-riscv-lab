"""
Database models and extension instances.
"""
from flask_sqlalchemy import SQLAlchemy
from flask_login import LoginManager, UserMixin
import hashlib
import os

db = SQLAlchemy()

login_manager = LoginManager()
login_manager.login_view = 'auth.login'


class User(UserMixin, db.Model):
    """User account model."""
    __tablename__ = 'users'

    id = db.Column(db.Integer, primary_key=True)
    username = db.Column(db.String(80), unique=True, nullable=False)
    password_hash = db.Column(db.String(128), nullable=False)
    created_at = db.Column(db.DateTime, server_default=db.func.now())

    def set_password(self, password):
        """Hash password with SHA-256 + salt (no bcrypt dependency)."""
        salt = os.urandom(16).hex()
        h = hashlib.sha256((salt + password).encode()).hexdigest()
        self.password_hash = f'{salt}${h}'

    def check_password(self, password):
        """Verify password against stored hash."""
        try:
            salt, stored_hash = self.password_hash.split('$', 1)
            h = hashlib.sha256((salt + password).encode()).hexdigest()
            return h == stored_hash
        except (ValueError, AttributeError):
            return False

    def __repr__(self):
        return f'<User {self.username}>'


class Post(db.Model):
    """Blog post / content model."""
    __tablename__ = 'posts'

    id = db.Column(db.Integer, primary_key=True)
    title = db.Column(db.String(200), nullable=False)
    body = db.Column(db.Text, nullable=False)
    author_id = db.Column(db.Integer, db.ForeignKey('users.id'), nullable=False)
    created_at = db.Column(db.DateTime, server_default=db.func.now())
    updated_at = db.Column(db.DateTime, server_default=db.func.now(),
                           onupdate=db.func.now())

    author = db.relationship('User', backref=db.backref('posts', lazy=True))

    def __repr__(self):
        return f'<Post {self.title!r}>'


@login_manager.user_loader
def load_user(user_id):
    return db.session.get(User, int(user_id))
