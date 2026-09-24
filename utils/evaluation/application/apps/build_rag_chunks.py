#!/usr/bin/env python3
# Build a real RAG chunk store: load the doc corpus, split with LlamaIndex's
# SentenceSplitter (real RAG chunking), write each chunk as a small file in
# sharded dirs.  The store is what a RAG engine loads cold to (re)build its
# index -- many small chunk files, open-dominated.  Usage: build <corpus> <out> [chunk_tokens]
import sys, os, hashlib
from llama_index.core import SimpleDirectoryReader
from llama_index.core.node_parser import SentenceSplitter
corpus, out = sys.argv[1], sys.argv[2]
ctok = int(sys.argv[3]) if len(sys.argv) > 3 else 96
docs = SimpleDirectoryReader(corpus, recursive=True, errors="ignore",
        required_exts=[".md",".rst",".txt",".markdown"]).load_data()
sp = SentenceSplitter(chunk_size=ctok, chunk_overlap=0)
nodes = sp.get_nodes_from_documents(docs)
os.makedirs(out, exist_ok=True)
n = 0
for nd in nodes:
    h = hashlib.md5((nd.node_id).encode()).hexdigest()
    d = os.path.join(out, h[:2], h[2:4]); os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, h+".txt"), "w") as f:
        f.write(nd.get_content())
    n += 1
print(f"wrote {n} chunk files to {out}")
