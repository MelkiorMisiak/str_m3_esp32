#!/usr/bin/env bash
# Experimento A: mede cobertura de linha e de desvio de main/huffman.c com gcov.
# Uso: cd host && ./run_coverage.sh
set -euo pipefail
cd "$(dirname "$0")"
rm -rf build && mkdir build && cd build

# -O0 + --coverage: sem otimizacao para que a cobertura reflita o codigo-fonte.
gcc -O0 -g --coverage -I../../main -c ../../main/huffman.c -o huffman.o
gcc -O0 -g -I../../main -c ../test_huffman.c -o test_huffman.o
gcc --coverage huffman.o test_huffman.o -o test_huffman

./test_huffman | tee test_output.txt

echo
echo "== gcov (linhas e desvios de huffman.c) =="
gcov -b -c huffman.o | tee gcov_summary.txt
echo
echo "Arquivo anotado: host/build/huffman.c.gcov"
echo "Cobertura de linha/desvio = valores 'Lines executed' e 'Taken at least once'."
