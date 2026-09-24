#!/bin/bash
# make_corpus.sh [DIR] -- a document corpus from open-source documentation
# sites: many small markdown files in deep trees (default DIR:
# datasets/build/corpus).
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
MASTER=${1:-$HERE/build/corpus}
mkdir -p "$MASTER"
DOCS=(
  "https://github.com/kubernetes/website"          # ~20k+ markdown, many langs
  "https://github.com/MicrosoftDocs/azure-docs"    # very large doc corpus
  "https://github.com/gitlabhq/gitlab-foss"        # docs/ tree of markdown
)
for url in "${DOCS[@]}"; do
  name=$(basename "$url")
  if [ -d "$MASTER/$name/.git" ]; then echo "  have $name"; continue; fi
  echo "  clone $name"
  git clone --quiet --depth 1 "$url" "$MASTER/$name" || echo "  WARN: $name"
done
echo "corpus master: $MASTER"
echo "  markdown/text files: $(find "$MASTER" -type f \( -name '*.md' -o -name '*.rst' -o -name '*.txt' \) | wc -l)"
echo "  total files: $(find "$MASTER" -type f | wc -l)"
echo "  dirs: $(find "$MASTER" -type d | wc -l)"
