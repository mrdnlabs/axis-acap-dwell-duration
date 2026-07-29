ARG ARCH=aarch64
ARG VERSION=12.11.0
ARG UBUNTU_VERSION=24.04
ARG REPO=axisecp
ARG SDK=acap-native-sdk

FROM ${REPO}/${SDK}:${VERSION}-${ARCH}-ubuntu${UBUNTU_VERSION}

COPY ./app /opt/app/
WORKDIR /opt/app

# Source copied from a Windows/WSL filesystem arrives as mode 777. The device
# then silently refuses to create the reverse-proxy rules, with nothing logged.
# Normalise permissions before building.
RUN find . -type f -exec chmod 644 {} + && \
    find . -type d -exec chmod 755 {} +

RUN . /opt/axis/acapsdk/environment-setup* && \
    acap-build .
