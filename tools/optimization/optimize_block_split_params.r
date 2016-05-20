#!/usr/bin/Rscript

PARAMS = read.table("block_split_params.txt", header=TRUE, row.names=1)

fn = function(params) {

    params = as.integer(params)
    params = pmax(params, PARAMS$MIN)
    params = pmin(params, PARAMS$MAX)

    env = paste(rownames(PARAMS), "=", params, sep='', collapse=' ')

    cat(env, "\n")

    result = system(paste(env, "./try_block_params.sh"), intern=TRUE)
    stopifnot(attr(result, "status") == 0)
    result = as.numeric(result)
    cat(result, "\n")
    return(result)
}

result = optim(PARAMS$INITIAL, fn)
print(result)
