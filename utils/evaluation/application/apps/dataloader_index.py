#!/usr/bin/env python3
"""A2: a PyTorch DataLoader's cold pass before training.  An ImageFolder-style
dataset walks every class directory and stats every file, then the loader
opens each image and reads a 4 KB header (no decode).

Usage: dataloader_index.py <root> [max_batches]; prints 'seconds=<wall>'.
"""
import os, sys, time
import torch
from torch.utils.data import Dataset, DataLoader


class ImageFolderMeta(Dataset):
    """Mirrors torchvision.datasets.ImageFolder init: walk class dirs,
    stat every entry to build the sample list (the cold metadata pass)."""
    def __init__(self, root):
        self.samples = []
        classes = sorted(e.name for e in os.scandir(root) if e.is_dir())
        for c in classes:
            d = os.path.join(root, c)
            for e in os.scandir(d):
                # stat each entry, exactly like ImageFolder's is_valid_file
                if e.is_file() and os.stat(e.path).st_size >= 0:
                    self.samples.append(e.path)

    def __len__(self):
        return len(self.samples)

    def __getitem__(self, i):
        p = self.samples[i]
        with open(p, "rb") as f:        # real open() on the test FS
            f.read(4096)
        return torch.zeros(1)


def main():
    root = sys.argv[1]
    # The metadata-bound stage a real training job pays cold is dataset
    # INITIALIZATION: torchvision ImageFolder.__init__ walks every class dir
    # and stat()s every file to build the sample list -- single-threaded and
    # latency-bound (the epoch's batch loading is then parallelized across
    # workers and becomes I/O-throughput-bound, which hides per-lookup cost).
    # We therefore time the single-threaded init walk as the stage.
    t0 = time.time()
    ds = ImageFolderMeta(root)                       # real ImageFolder init walk
    dt = time.time() - t0
    print(f"samples={len(ds)} seconds={dt:.3f}", file=sys.stderr)
    print(f"seconds={dt:.3f}")


if __name__ == "__main__":
    main()
