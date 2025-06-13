#!/bin/bash
# Check if the Docker image exists
IMAGE_NAME="xv6-riscv-container"
if [[ "$(docker images -q $IMAGE_NAME 2> /dev/null)" == "" ]]; then
    echo "Building $IMAGE_NAME Docker image..."
    
    # Build the Docker image from the Dockerfile in the current directory
    docker build -t $IMAGE_NAME .
    
    echo "Docker image $IMAGE_NAME built successfully!"
else
    echo "Docker image $IMAGE_NAME already exists."
fi

# Run the container with the current directory mounted
docker run -it --rm -v "$(pwd)":/xv6 $IMAGE_NAME
