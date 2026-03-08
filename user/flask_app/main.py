"""
Main blueprint — public pages, dashboard, posts, API.
"""
import platform
import os
from flask import Blueprint, render_template, redirect, url_for, flash, \
    request, jsonify
from flask_login import login_required, current_user
from models import db, Post

main_bp = Blueprint('main', __name__)


@main_bp.route('/')
def index():
    posts = Post.query.order_by(Post.created_at.desc()).limit(10).all()
    return render_template('index.html', posts=posts)


@main_bp.route('/about')
def about():
    return render_template('about.html')


@main_bp.route('/dashboard')
@login_required
def dashboard():
    posts = Post.query.filter_by(author_id=current_user.id) \
        .order_by(Post.created_at.desc()).all()
    return render_template('dashboard.html', posts=posts)


@main_bp.route('/post/new', methods=['GET', 'POST'])
@login_required
def new_post():
    if request.method == 'POST':
        title = request.form.get('title', '').strip()
        body = request.form.get('body', '').strip()
        if not title or not body:
            flash('Title and body are required.', 'error')
        else:
            post = Post(title=title, body=body, author_id=current_user.id)
            db.session.add(post)
            db.session.commit()
            flash('Post created!', 'success')
            return redirect(url_for('main.dashboard'))
    return render_template('edit_post.html', post=None)


@main_bp.route('/post/<int:post_id>')
def view_post(post_id):
    post = db.get_or_404(Post, post_id)
    return render_template('view_post.html', post=post)


@main_bp.route('/post/<int:post_id>/edit', methods=['GET', 'POST'])
@login_required
def edit_post(post_id):
    post = db.get_or_404(Post, post_id)
    if post.author_id != current_user.id:
        flash('You can only edit your own posts.', 'error')
        return redirect(url_for('main.index'))

    if request.method == 'POST':
        post.title = request.form.get('title', '').strip()
        post.body = request.form.get('body', '').strip()
        if not post.title or not post.body:
            flash('Title and body are required.', 'error')
        else:
            db.session.commit()
            flash('Post updated!', 'success')
            return redirect(url_for('main.view_post', post_id=post.id))

    return render_template('edit_post.html', post=post)


@main_bp.route('/post/<int:post_id>/delete', methods=['POST'])
@login_required
def delete_post(post_id):
    post = db.get_or_404(Post, post_id)
    if post.author_id != current_user.id:
        flash('You can only delete your own posts.', 'error')
        return redirect(url_for('main.index'))
    db.session.delete(post)
    db.session.commit()
    flash('Post deleted.', 'info')
    return redirect(url_for('main.dashboard'))


# ── REST API ──────────────────────────────────────────────────────────────────

@main_bp.route('/api/status')
def api_status():
    """System status endpoint."""
    try:
        uname = os.uname()
        sysname = uname.sysname
        machine = uname.machine
        release = uname.release
    except Exception:
        sysname = 'xv6'
        machine = 'unknown'
        release = 'unknown'

    return jsonify({
        'status': 'ok',
        'os': sysname,
        'arch': machine,
        'release': release,
        'python': platform.python_version(),
        'server': 'Flask on xv6',
    })


@main_bp.route('/api/posts')
def api_posts():
    """List posts as JSON."""
    posts = Post.query.order_by(Post.created_at.desc()).limit(50).all()
    return jsonify([{
        'id': p.id,
        'title': p.title,
        'body': p.body[:200],
        'author': p.author.username,
        'created_at': p.created_at.isoformat() if p.created_at else None,
    } for p in posts])
