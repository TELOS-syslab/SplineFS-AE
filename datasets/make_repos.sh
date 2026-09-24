#!/bin/bash
# make_repos.sh [DIR] -- shallow clones of large open-source repositories
# (default DIR: datasets/build/repos).
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
MASTER=${1:-$HERE/build/repos}
mkdir -p "$MASTER"
REPOS=(
  "https://github.com/django/django"
  "https://github.com/pallets/flask"
  "https://github.com/psf/requests"
  "https://github.com/numpy/numpy"
  "https://github.com/pandas-dev/pandas"
  "https://github.com/scikit-learn/scikit-learn"
  "https://github.com/pytest-dev/pytest"
  "https://github.com/encode/django-rest-framework"
  "https://github.com/sqlalchemy/sqlalchemy"
  "https://github.com/celery/celery"
)
for url in "${REPOS[@]}"; do
  name=$(basename "$url")
  if [ -d "$MASTER/$name/.git" ]; then echo "  have $name"; continue; fi
  echo "  clone $name"
  git clone --quiet --depth 1 "$url" "$MASTER/$name" \
    && (cd "$MASTER/$name" && git count-objects -v >/dev/null 2>&1) \
    || echo "  WARN clone failed: $name"
done
echo "repos master: $MASTER"
echo "  dirs:  $(find "$MASTER" -type d | wc -l)"
echo "  files: $(find "$MASTER" -type f | wc -l)"
