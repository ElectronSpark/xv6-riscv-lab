#!/bin/bash
# install_flask.sh — Download and install Flask + all dependencies into sysroot
#
# Usage: install_flask.sh <site-packages-dir> <download-cache-dir>
#
# Installs pure-Python packages: Flask, Werkzeug, Jinja2, itsdangerous,
# click, MarkupSafe, blinker, and optional full-stack extras: waitress,
# SQLAlchemy, Flask-Login, Flask-WTF, WTForms, Flask-Session, Mako,
# typing_extensions.
#
# Also downloads the Mozilla CA certificate bundle for TLS.

set -e

SITE_PACKAGES="$1"
DOWNLOAD_DIR="$2"

if [ -z "$SITE_PACKAGES" ] || [ -z "$DOWNLOAD_DIR" ]; then
    echo "Usage: $0 <site-packages-dir> <download-cache-dir>" >&2
    exit 1
fi

mkdir -p "$SITE_PACKAGES"
mkdir -p "$DOWNLOAD_DIR"

# ── Package versions (PyPI source distributions) ─────────────────────────────
# Each entry: NAME  VERSION  PYPI_PROJECT  INNER_DIR
# INNER_DIR is the directory inside the tarball that contains the Python package.
# For most packages it's just the lowercase package name.
declare -A PACKAGES=(
    # Flask core + dependencies
    [flask]="3.1.0"
    [werkzeug]="3.1.3"
    [jinja2]="3.1.5"
    [itsdangerous]="2.2.0"
    [click]="8.1.8"
    [markupsafe]="3.0.2"
    [blinker]="1.9.0"

    # Production WSGI server
    [waitress]="3.0.2"

    # ORM / Database
    [sqlalchemy]="2.0.37"
    [typing_extensions]="4.12.2"

    # Flask extensions
    [flask_login]="0.6.3"
    [flask_sqlalchemy]="3.1.1"
    [flask_wtf]="1.2.2"
    [wtforms]="3.2.1"
    [flask_session]="0.8.0"
    [mako]="1.3.8"
)

# Map package key -> PyPI project name (for URL) and importable directory name
declare -A PYPI_NAMES=(
    [flask]="flask"
    [werkzeug]="werkzeug"
    [jinja2]="jinja2"
    [itsdangerous]="itsdangerous"
    [click]="click"
    [markupsafe]="markupsafe"
    [blinker]="blinker"
    [waitress]="waitress"
    [sqlalchemy]="sqlalchemy"
    [typing_extensions]="typing_extensions"
    [flask_login]="flask-login"
    [flask_sqlalchemy]="flask-sqlalchemy"
    [flask_wtf]="flask-wtf"
    [wtforms]="wtforms"
    [flask_session]="flask-session"
    [mako]="mako"
)

# Map package key -> importable package directory name inside the sdist
declare -A IMPORT_NAMES=(
    [flask]="flask"
    [werkzeug]="werkzeug"
    [jinja2]="jinja2"
    [itsdangerous]="itsdangerous"
    [click]="click"
    [markupsafe]="markupsafe"
    [blinker]="blinker"
    [waitress]="waitress"
    [sqlalchemy]="sqlalchemy"
    [typing_extensions]="typing_extensions"
    [flask_login]="flask_login"
    [flask_sqlalchemy]="flask_sqlalchemy"
    [flask_wtf]="flask_wtf"
    [wtforms]="wtforms"
    [flask_session]="flask_session"
    [mako]="mako"
)

# PyPI sdist URL format:
# https://files.pythonhosted.org/packages/source/<first-letter>/<project>/<project>-<version>.tar.gz
# But the actual extraction path varies. We'll use pip download format.

# Create a minimal .dist-info directory for importlib.metadata
_create_dist_info() {
    local key="$1" version="$2" pypi_name="$3" import_name="$4"
    # PEP 427: dist-info directory name uses the distribution name (with
    # hyphens replaced by underscores) and version.
    local safe_name="${pypi_name//-/_}"
    local dist_dir="${SITE_PACKAGES}/${safe_name}-${version}.dist-info"
    mkdir -p "$dist_dir"
    cat > "$dist_dir/METADATA" <<EOF
Metadata-Version: 2.1
Name: ${pypi_name}
Version: ${version}
EOF
    # INSTALLER
    echo "install_flask.sh" > "$dist_dir/INSTALLER"
    # RECORD (empty — not strictly needed but some tools expect it)
    touch "$dist_dir/RECORD"
    # top_level.txt
    echo "$import_name" > "$dist_dir/top_level.txt"
}

download_and_install() {
    local key="$1"
    local version="${PACKAGES[$key]}"
    local pypi_name="${PYPI_NAMES[$key]}"
    local import_name="${IMPORT_NAMES[$key]}"

    local tarball_path="${DOWNLOAD_DIR}/${pypi_name}-${version}.tar.gz"

    # Check if already installed (version marker)
    local marker="${SITE_PACKAGES}/.${key}-${version}.installed"
    if [ -f "$marker" ]; then
        echo "  [skip] ${pypi_name} ${version} (already installed)"
        return 0
    fi

    # Download if not cached
    if [ ! -f "$tarball_path" ]; then
        # Use PyPI JSON API to get the actual sdist URL (handles hash-based paths)
        echo "  [resolve] ${pypi_name} ${version}"
        local sdist_url
        sdist_url=$(curl -fsSL "https://pypi.org/pypi/${pypi_name}/${version}/json" \
            | python3 -c "
import sys, json
data = json.load(sys.stdin)
for u in data.get('urls', []):
    if u.get('packagetype') == 'sdist':
        print(u['url'])
        break
" 2>/dev/null)

        if [ -z "$sdist_url" ]; then
            echo "ERROR: Could not resolve sdist URL for ${pypi_name} ${version}" >&2
            return 1
        fi

        echo "  [download] ${sdist_url##*/}"
        if ! curl -fsSL --retry 3 -o "$tarball_path" "$sdist_url"; then
            echo "ERROR: Failed to download ${pypi_name} ${version}" >&2
            rm -f "$tarball_path"
            return 1
        fi
    else
        echo "  [cached] ${pypi_name}-${version}.tar.gz"
    fi

    # Extract to a temporary directory
    local extract_dir
    extract_dir=$(mktemp -d)
    tar -xzf "$tarball_path" -C "$extract_dir"

    # Find the package directory inside the extracted sdist.
    # The sdist contains: <project>-<version>/src/<import_name>/ (modern layout)
    #                  or: <project>-<version>/<import_name>/     (classic layout)
    local sdist_root
    sdist_root=$(find "$extract_dir" -maxdepth 1 -mindepth 1 -type d | head -1)

    local pkg_src=""
    # Try modern src layout first
    if [ -d "$sdist_root/src/$import_name" ]; then
        pkg_src="$sdist_root/src/$import_name"
    elif [ -d "$sdist_root/$import_name" ]; then
        pkg_src="$sdist_root/$import_name"
    elif [ -d "$sdist_root/lib/$import_name" ]; then
        # Some packages (e.g., SQLAlchemy) use a lib/ layout
        pkg_src="$sdist_root/lib/$import_name"
    else
        # For single-file packages (like typing_extensions)
        if [ -f "$sdist_root/src/${import_name}.py" ]; then
            cp "$sdist_root/src/${import_name}.py" "$SITE_PACKAGES/"
            _create_dist_info "$key" "$version" "$pypi_name" "$import_name"
            touch "$marker"
            rm -rf "$extract_dir"
            echo "  [installed] ${import_name}.py ${version}"
            return 0
        elif [ -f "$sdist_root/${import_name}.py" ]; then
            cp "$sdist_root/${import_name}.py" "$SITE_PACKAGES/"
            _create_dist_info "$key" "$version" "$pypi_name" "$import_name"
            touch "$marker"
            rm -rf "$extract_dir"
            echo "  [installed] ${import_name}.py ${version}"
            return 0
        else
            echo "ERROR: Cannot find ${import_name} in ${sdist_root}" >&2
            echo "  Contents: $(ls "$sdist_root")" >&2
            rm -rf "$extract_dir"
            return 1
        fi
    fi

    # Copy the package directory, excluding C source files (use pure-Python fallbacks)
    rsync -a \
        --exclude='*.c' \
        --exclude='*.h' \
        --exclude='__pycache__' \
        --exclude='*.so' \
        "$pkg_src/" "$SITE_PACKAGES/$import_name/"

    # Create .dist-info so importlib.metadata can find the package
    _create_dist_info "$key" "$version" "$pypi_name" "$import_name"

    touch "$marker"
    rm -rf "$extract_dir"
    echo "  [installed] ${import_name}/ ${version}"
}

# ── Download Mozilla CA certificate bundle ───────────────────────────────────
download_ca_bundle() {
    local ca_cert_dir="${SITE_PACKAGES}/../../../etc-ssl-certs"
    mkdir -p "$ca_cert_dir"
    local ca_file="${ca_cert_dir}/ca-certificates.crt"

    if [ -f "$ca_file" ]; then
        echo "  [skip] CA certificate bundle (already downloaded)"
        return 0
    fi

    echo "  [download] Mozilla CA certificate bundle"
    curl -fsSL --retry 3 -o "$ca_file" \
        "https://curl.se/ca/cacert.pem" || {
        echo "WARNING: Failed to download CA bundle (HTTPS verification will fail)" >&2
        return 0  # non-fatal
    }
    echo "  [installed] ca-certificates.crt ($(wc -l < "$ca_file") lines)"
}

# ── Main ─────────────────────────────────────────────────────────────────────
echo "install_flask: Installing Flask + dependencies into ${SITE_PACKAGES}"
echo ""

# Install in dependency order
echo "=== Flask core dependencies ==="
for pkg in markupsafe jinja2 itsdangerous click blinker werkzeug flask; do
    download_and_install "$pkg"
done

echo ""
echo "=== Production WSGI server ==="
download_and_install waitress

echo ""
echo "=== ORM / Database ==="
for pkg in typing_extensions sqlalchemy; do
    download_and_install "$pkg"
done

echo ""
echo "=== Flask extensions ==="
for pkg in flask_login flask_sqlalchemy flask_wtf wtforms flask_session mako; do
    download_and_install "$pkg"
done

echo ""
echo "=== CA certificates ==="
download_ca_bundle

echo ""
echo "install_flask: Done. $(find "$SITE_PACKAGES" -name '*.py' -type f | wc -l) Python files installed."
