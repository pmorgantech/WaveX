#!/usr/bin/env bash
set -euo pipefail

root="$(git rev-parse --show-toplevel 2>/dev/null || pwd)"
cd "$root"

# NO LLM!
unset GRAPHIFY_BACKEND GRAPHIFY_MODEL
unset OPENAI_API_KEY OPENAI_BASE_URL OPENAI_MODEL
unset ANTHROPIC_API_KEY ANTHROPIC_MODEL
unset GEMINI_API_KEY GOOGLE_API_KEY GOOGLE_MODEL

graphify update --no-viz .

echo
echo "Graph updated:"
echo "  graphify-out/GRAPH_REPORT.md"
echo "  graphify-out/graph.html"
echo "  graphify-out/graph.json"
