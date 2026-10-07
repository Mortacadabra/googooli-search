# indexer/tokenizer.py
import re


TOKEN_PATTERN = re.compile(r"[A-Za-z0-9]+")

def tokenize(text: str) -> list[str]:
    return TOKEN_PATTERN.findall(text.lower())
