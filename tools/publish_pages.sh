#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Publish site/ (web/make_site.py) on GitHub Pages: the gh-pages branch is site/ as its root.
#   tools/publish_pages.sh        after committing the new site/ on main
set -e
cd "$(dirname "$0")/.."
git subtree split --prefix site -b gh-pages-tmp >/dev/null
git push --force origin gh-pages-tmp:gh-pages
git branch -D gh-pages-tmp >/dev/null
