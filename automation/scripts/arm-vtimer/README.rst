.. SPDX-License-Identifier: GPL-2.0-only

Arm virtual-timer tests
=======================

An Arm guest uses its virtual timer to wake up at a deadline. Before the
fix, an early timer IRQ could leave the timer masked. Linux could then
miss the next wake-up. The fix checks the timer status before Xen masks
the timer or raises the guest IRQ.

A push to ``test/arm-vtimer-ci`` selects jobs with ``vtimer`` in their
names. One job builds debug Xen with performance counters from the base
and pipeline commits. Another builds two Zephyr guests. Five tests boot
guests in QEMU with GICv3. A sixth compares the two handlers on the host.

What changed in Xen?
--------------------

``arm-vtimer-compare`` takes ``vtimer_interrupt()`` from the commit before
the fix and from the pipeline commit. It compiles each copy with a model
of an enabled, unmasked timer and a guest IRQ line. It calls each handler
once per row, starting with a fresh timer each time.

====================  ==============================  ==============================
Timer status          Before the fix                  With the fix
====================  ==============================  ==============================
Deadline reached      Mask timer and raise guest IRQ  Mask timer and raise guest IRQ
Deadline still ahead  Mask timer and raise guest IRQ  Leave timer and IRQ alone
====================  ==============================  ==============================

The job counts writes to the timer and calls to the guest IRQ line.
Its log gives both Git revisions.

How does QEMU produce an early IRQ?
-----------------------------------

#. The interrupt controller (GIC) and the timer have separate state.
   QEMU's device test interface, qtest, can mark the timer IRQ pending
   in the GIC while the guest runs. This leaves the timer deadline alone.
   When Xen handles that IRQ, it reads the timer's own status bit,
   ``ISTATUS``. For an enabled timer, a clear bit means its deadline is
   still ahead. This needs no GDB or breakpoint.
#. The runner waits for guest output so the request comes during the
   test. Linux reports that it is ready, waits for the runner to reply,
   then prints a marker as it starts the timed waits. Zephyr prints its
   ready marker after it starts a one-shot timer. The runner waits a short
   time after the relevant marker. This delay gives the guest time to
   enter the check. Host and guest timing can vary on each run.
#. QEMU replies ``OK`` when it accepts a request. At the end, the runner
   reads Xen's debug counters. Each test that makes a request needs a
   positive count of timer IRQs that Xen handled with status clear. The
   count covers the whole boot and cannot identify an individual request.
   The guest checks below show whether time kept advancing. Finding the
   cause on physical hardware takes a separate measurement.

What do the guests check?
-------------------------

**Linux** uses the same kernel, rootfs and guest script in all three
jobs. One task sleeps 30 times. A busy loop keeps the pinned guest vCPU
runnable. Linux must report all 30 wake-ups in order and finish within
the 60-second test limit. Both control jobs run without a forced IRQ.

==================  ===============  ========================================
Linux case          Xen image        What the job checks
==================  ===============  ========================================
``before-control``  Before the fix   Timed waits work under normal conditions
``after-control``   Pipeline commit  Timed waits still work with the fix
``after-irq``       Pipeline commit  Timed waits finish after one IRQ request
==================  ===============  ========================================

**Zephyr v4.5.0** runs on the pipeline Xen image in two jobs. In
periodic mode the kernel ticks at regular intervals. In tickless mode
it sets its next deadline as needed. ``zephyr-4.5.0-arm64-vtimer`` builds
both modes from the probe source in ``zephyr/``, using the official
Zephyr CI base image on an Arm64 runner. The build script pins Zephyr
and Xenlib. It publishes binaries, configs and checksums under
``binaries/zephyr/``. Only the Zephyr test jobs need that artifact.
Each checks its config and makes 40 IRQ requests. A guest wrapper
reads ``ISTATUS`` before calling the standard timer handler. It must
see at least one timer IRQ and zero with status clear. The guest checks
its one-shot wake-up against the Arm counter, then runs a periodic
timer through 30 timed sleeps and requires at least 20 expiries.

Where are the results?
----------------------

Share the URL of the completed pipeline with the patch reply. The QEMU
jobs print ``CI_PIPELINE_URL`` in their logs and publish a JUnit result.
The ``vtimer-results/`` artifacts include ``injections.log`` for QEMU
replies and ``console.log`` for guest output and Xen counters.

To run the guest tests locally, put the CI build artifacts in
``binaries/`` and run from the Xen tree. Linux needs the kernel and rootfs
in ``binaries/Image`` and ``binaries/rootfs.cpio.gz``. For Zephyr, unpack
the guest build artifact into the same tree, or run
``bash automation/scripts/arm-vtimer/build-zephyr.sh`` in its build image.
Give each test a fresh output directory.

.. code-block:: bash

    runner=automation/scripts/arm-vtimer/run.sh
    OUT=vtimer-before bash "$runner" linux before-control
    OUT=vtimer-after bash "$runner" linux after-control
    OUT=vtimer-irq bash "$runner" linux after-irq
    OUT=vtimer-periodic bash "$runner" zephyr periodic
    OUT=vtimer-tickless bash "$runner" zephyr tickless
