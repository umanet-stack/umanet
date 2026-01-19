BEGIN {
        start = 0
        lb = 0
        lb_p = 0
        ub = 0
        ub_p = 0
}
{
    if ($1 == "") {
            next
    }
    if (!start) {
        if ($1 == "Value") {
            start = 1
        }
        next
    }
    if ($2 == "=") {
        exit
    }
    if ($2 > ARGV[2]) {
        ub = $1
        ub_p = $2
        exit
    }
    lb = $1
    lb_p = $2
    # print $1, $2
}
END {
    # print lb_p, lb
    # print ub_p, ub
    printf "%.3f", ((ARGV[2] - lb_p) * ub + (ub_p - ARGV[2]) * lb) / (ub_p - lb_p)
}
