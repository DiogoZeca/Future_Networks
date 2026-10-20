#!/bin/sh
# gen-cert.sh — gera o certificado auto-assinado do servidor (M6).
#
# O slide 123 pede "TLS with a self-signed certificate". Auto-assinado quer
# dizer que não há autoridade nenhuma a garantir quem é o servidor: o
# certificado diz "sou o future-networks" e assina essa afirmação com a sua
# própria chave. Qualquer pessoa pode gerar um igual.
#
# É por isso que o cliente NÃO valida a cadeia (não há cadeia) e valida o
# FINGERPRINT: o SHA-256 do certificado, que este script imprime no fim. Quem
# se quiser ligar com segurança recebe esse fingerprint por outro meio (a
# mensagem onde se combina o teste, o quadro da aula) e passa-o em -f.
#
# Sem isso o TLS cifrava o tráfego mas não autenticava ninguém: um atacante no
# meio apresentava o certificado DELE e o cliente aceitava. Ver docs/milestones/M6.md §D-TLS-2.
#
# Uso:
#   scripts/gen-cert.sh [diretorio]     (por omissão: certs/)
set -eu

DIR="${1:-certs}"
DAYS=365
KEY="$DIR/server.key"
CRT="$DIR/server.pem"

if ! command -v openssl >/dev/null 2>&1; then
    echo "erro: o openssl nao esta instalado" >&2
    echo "  Arch:   sudo pacman -S openssl" >&2
    echo "  Debian: sudo apt install openssl" >&2
    exit 1
fi

# O .gitignore tem de ignorar isto ANTES de existir. Se alguém correr este
# script num repositório sem essa linha, a chave privada fica a um `git add .`
# de distância de ir para o histórico — e dali não sai mais.
# (Testa-se com um caminho de FICHEIRO lá dentro: um padrão "certs/" não casa
# com "certs" enquanto o diretório não existir.)
if [ -d .git ] && git check-ignore -q "$KEY" 2>/dev/null; then
    :
elif [ -d .git ]; then
    echo "erro: '$DIR' nao esta no .gitignore." >&2
    echo "A chave privada NAO pode ir para o repositorio. Acrescenta:" >&2
    echo "    certs/" >&2
    echo "    *.pem" >&2
    echo "    *.key" >&2
    exit 1
fi

mkdir -p "$DIR"

if [ -f "$KEY" ] || [ -f "$CRT" ]; then
    echo "ja existe um certificado em $DIR/ (apaga-o primeiro para gerar outro)"
else
    # -nodes: a chave fica sem senha. Num servidor a sério seria o contrário,
    #   mas aqui o servidor arranca sem ninguém à frente para a escrever.
    # subjectAltName: o CN sozinho já não é aceite por implementações
    #   modernas. Sem SAN, um cliente que valide o NOME (o de outro grupo, ou
    #   o `openssl s_client`) recusa logo. O nosso valida o fingerprint, que
    #   é independente do nome — mas não vale a pena gerar um certificado que
    #   só nós aceitamos.
    openssl req -x509 -newkey rsa:2048 -nodes -days "$DAYS" \
        -keyout "$KEY" -out "$CRT" \
        -subj "/CN=future-networks" \
        -addext "subjectAltName=DNS:localhost,IP:127.0.0.1,IP:::1" \
        2>/dev/null

    chmod 600 "$KEY"        # só o dono lê a chave privada
    chmod 644 "$CRT"
    echo "gerado:"
    echo "  $CRT   (certificado - publico)"
    echo "  $KEY   (chave privada - NAO partilhar, NAO commitar)"
fi

echo
echo "valido ate:"
openssl x509 -in "$CRT" -noout -enddate | sed 's/notAfter=/  /'
echo
echo "fingerprint SHA-256 (e isto que o cliente precisa):"
FP=$(openssl x509 -in "$CRT" -noout -fingerprint -sha256 | cut -d= -f2)
echo "  $FP"
echo
echo "para ligar:"
echo "  ./bin/client -f $FP 127.0.0.1 5443 ana"
