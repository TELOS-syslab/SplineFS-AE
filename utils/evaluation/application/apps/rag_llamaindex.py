#!/usr/bin/env python3
# Real RAG ingest stage: LlamaIndex SimpleDirectoryReader walks the corpus and
# loads every document (the cold metadata+read pass a RAG engine pays before it
# can chunk/embed).  Prints seconds=<wall>.  Usage: real_rag_llamaindex.py <dir>
import sys, time
from llama_index.core import SimpleDirectoryReader
root = sys.argv[1]
t0 = time.time()
docs = SimpleDirectoryReader(root, recursive=True, errors="ignore",
                             required_exts=[".md",".rst",".txt",".markdown",".json",".yaml",".yml"]).load_data()
print(f"loaded {len(docs)} docs seconds={time.time()-t0:.3f}", flush=True)
