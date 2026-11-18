# SPDX-License-Identifier: GPL-2.0-only
function fail(reason) { if (!error) error = reason }
{
    gsub(/\r/, ""); gsub(/\033\[[0-9;]*[[:alpha:]]/, "")
    if (/VTIMER_READY/) ready = 1
    if (/^VTIMER_MEASURE$/) measure = 1
    if (/^VTIMER_TICK_[0-9]+$/) {
        n = $0; sub(/^VTIMER_TICK_/, "", n)
        if (!measure || n + 0 != ticks) fail("Wrong Linux wait order")
        ticks++
    }
    if (/^VTIMER_LINUX_PASS$/) linux_pass = 1
    if (/Kernel panic|VTIMER_PROBE_FAIL/) fail("Guest failed")
    if (/VTIMER_PROBE_PASS/) {
        probes++
        if ($0 !~ /^\(d1\) VTIMER_PROBE_PASS expiries=[0-9]+ irqs=[0-9]+ spurious=[0-9]+$/)
            fail("Unexpected Zephyr result")
        split($3, e, "="); split($4, q, "="); split($5, s, "=")
        expiries = e[2] + 0; guest_irqs = q[2] + 0; spurious = s[2] + 0
    }
    if (/^\(XEN\) Virtual timer spurious interrupts[[:space:]]+TOTAL\[[[:space:]]*[0-9]+\]/) {
        value = $0; sub(/.*TOTAL\[[[:space:]]*/, "", value); filtered = value + 0
    }
}
END {
    if (guest == "linux") {
        printf "Linux reported %d/30 timed waits.\n", ticks > "/dev/stderr"
        if (!ready || !measure || ticks != 30 || !linux_pass) fail("Linux did not finish all waits")
    } else {
        if (probes == 1)
            printf "Zephyr reported %d periodic expiries, %d timer IRQs, %d with ISTATUS clear.\n", \
                   expiries, guest_irqs, spurious > "/dev/stderr"
        if (!ready || probes != 1 || expiries < 20 || !guest_irqs || spurious)
            fail("Missing timer progress or spurious IRQ seen by Zephyr")
    }
    if (requests) {
        printf "Xen spurious timer count for this run = %d.\n", filtered > "/dev/stderr"
        if (!filtered) fail("Missing Xen filter evidence")
    }
    if (control_status) fail("Console test failed; see run.log")
    printf "<testsuites><testsuite name=\"%s\"><testcase classname=\"%s\" name=\"timer_progress\">", name, name
    if (error) { printf "<failure>%s</failure>", error; print error > "/dev/stderr" }
    print "</testcase></testsuite></testsuites>"
    printf "%s\t%s\ttimer_progress\n", name, (error ? "FAIL" : "PASS") > "tests.tsv"
    exit (error != "")
}
