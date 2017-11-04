#include <linux/module.h>
#include <linux/printk.h>
#include <linux/device.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/fpga/fpga-mgr.h>

MODULE_LICENSE("Dual BSD/GPL");

struct fpga_prog_dev;

static const  char  *class_name      = "fpga_prog";
static const  char  *of_name         = "fpga-prog";
static struct class *fpga_prog_class = 0;

static DEFINE_IDA(fpga_prog_ida);

static ssize_t
bind_store(struct class *cls, struct class_attribute *att, const char *buf, size_t sz);

static ssize_t
remove_store(struct device *dev, struct device_attribute *att, const char *buf, size_t sz);

static ssize_t
file_store(struct device *dev, struct device_attribute *att, const char *buf, size_t sz);
static ssize_t
file_show(struct device *dev, struct device_attribute *att, char *buf);

static ssize_t
program_store(struct device *dev, struct device_attribute *att, const char *buf, size_t sz);

static ssize_t
autoload_store(struct device *dev, struct device_attribute *att, const char *buf, size_t sz);
static ssize_t
autoload_show(struct device *dev, struct device_attribute *att, char *buf);


static int load_fw(struct fpga_prog_dev *prg);

static void dev_release(struct device *dev);

CLASS_ATTR_WO( bind );

DEVICE_ATTR_WO( remove   );
DEVICE_ATTR_RW( file     );
DEVICE_ATTR_WO( program  );
DEVICE_ATTR_RW( autoload );

static struct device_attribute *dev_attrs[] = {
	&dev_attr_remove,
	&dev_attr_program,
	&dev_attr_file,
	&dev_attr_autoload,
};

#define N_DEV_ATTRS (sizeof(dev_attrs)/sizeof(dev_attrs[0]))

#define MAXLEN 1023

struct fpga_prog_dev {
	struct device dev;
	const char *  file;
	int           autoload;
};

static struct fpga_manager *of_get_mgr_from_dev(struct device *devp)
{
struct device_node  *pnod = devp->of_node;
struct device_node  *mnod;
struct fpga_manager *mgr  = 0;

	if ( ! pnod )
		return ERR_PTR( -ENODEV );

	of_node_get( pnod );
		if ( of_device_is_compatible(pnod, of_name) ) {
			mnod = of_parse_phandle(pnod, "fpga-mgr", 0);
			if ( mnod ) {
				mgr = of_fpga_mgr_get( mnod );
			}
		}
	of_node_put( pnod );
	return mgr;
}

static struct fpga_manager *of_get_mgr_from_path(const char *path)
{
struct device_node  *mnod = of_find_node_by_path( path );
struct fpga_manager *mgr  = 0;

	if ( ! mnod )
		return ERR_PTR( -ENOENT );

	mgr = of_fpga_mgr_get( mnod );

	of_node_put( mnod );

	return mgr;
}


static int cmp_of_node(struct device *dev, const void *data)
{
	return (const void *)dev->of_node == data;
}

static int load_fw(struct fpga_prog_dev *prg)
{
struct fpga_manager   *mgr;
int                    err;
struct fpga_image_info info;

	info.flags                      = 0;
	info.enable_timeout_us          = 1000000;
	info.disable_timeout_us         = 1000000;
	info.config_complete_timeout_us = 1000000;

	if ( ! prg->file )
		return -EINVAL;

	mgr = of_fpga_mgr_get( prg->dev.of_node );

	if ( IS_ERR( mgr ) ) {
		err = PTR_ERR( mgr );
	} else {
		err = fpga_mgr_firmware_load( mgr, &info , "/mnt/zybo_generic.bin" );
		fpga_mgr_put( mgr );
	}

	return err;
}

static struct fpga_prog_dev *create_dev(struct fpga_manager *mgr)
{
struct fpga_prog_dev *prog;
void                 *mem  = 0;
int                   id   = -1;
int                   stat;
int                   dev_attr_stat[ N_DEV_ATTRS ];
int                   i;

	for ( i=0; i<N_DEV_ATTRS; i++ ) {
		dev_attr_stat[i] = -1;
	}

	if ( class_find_device( fpga_prog_class, 0, mgr->dev.of_node, cmp_of_node ) ) {
		return ERR_PTR(-EEXIST);
	}

	if ( ! ( prog = kzalloc( sizeof(*prog), GFP_KERNEL ) ) ) {
		return ERR_PTR(-ENOMEM);
	}

	mem = (void*)prog;

	id = ida_simple_get( &fpga_prog_ida, 0, 0, GFP_KERNEL );
	if ( id < 0 ) {
		prog = ERR_PTR( id );
		goto bail;
	}

	prog->autoload     = 1;

	device_initialize( &prog->dev );
	prog->dev.class    = fpga_prog_class;
	prog->dev.parent   = mgr->dev.parent;
	prog->dev.of_node  = mgr->dev.of_node;
	prog->dev.id       = id;
	prog->dev.release  = dev_release;
	dev_set_drvdata( &prog->dev, 0 );

	stat = dev_set_name( &prog->dev, "prog-%s", dev_name( &mgr->dev ) );
	if ( stat ) {
		prog = ERR_PTR( stat );
		goto bail;
	}

	stat = device_add( &prog->dev );
	if ( stat ) {
		prog = ERR_PTR( stat );
		goto bail;
	}

	for ( i=0; i<N_DEV_ATTRS; i++ ) {
		if ( (dev_attr_stat[i] = device_create_file( &prog->dev, dev_attrs[i] )) )
			break;
	}
	if ( i < N_DEV_ATTRS ) {
		prog = ERR_PTR( dev_attr_stat[i] );
		goto bail;
	}

	dev_attr_stat[0] = -1;
	mem              =  0;
	id               = -1;

bail:

	for ( i=0; i < N_DEV_ATTRS && dev_attr_stat[i] == 0; i++ ) {
		device_remove_file( &prog->dev, dev_attrs[i] );
	}
	
	if ( id >= 0 ) {
		ida_simple_remove( &fpga_prog_ida, id );
	}

	if ( mem ) {
		kfree( mem );
	}
	return prog;
}

static void dev_release(struct device *dev)
{
struct fpga_prog_dev *prg = container_of( dev, struct fpga_prog_dev, dev );

	ida_simple_remove( &fpga_prog_ida, prg->dev.id );

	if ( prg->file )
		kfree( prg->file );

	kfree( prg );
}

static ssize_t
bind_store(struct class *cls, struct class_attribute *att, const char *buf, size_t sz)
{
struct fpga_manager  *mgr;
struct fpga_prog_dev *prg;

	if ( buf[sz] )
		return -EINVAL;


	if ( sz > MAXLEN ) {
		return -ENOMEM;
	}
	mgr = of_get_mgr_from_path( buf );

	if ( IS_ERR( mgr ) ) {
		return PTR_ERR( mgr );
	}

	prg = create_dev( mgr );

	fpga_mgr_put( mgr );

	if ( IS_ERR( prg ) ) {
		return PTR_ERR( prg );
	}

	printk(KERN_DEBUG "release: %p\n", prg->dev.release);

	return sz;
}

static ssize_t
remove_store(struct device *dev, struct device_attribute *att, const char *buf, size_t sz)
{
int val;

	if ( kstrtoint(buf, 0, &val) ) {
		return -EINVAL;
	}

	if ( val && device_remove_file_self( dev, att ) ) {
		device_unregister( dev );
	}

	return sz;
}

static ssize_t
file_store(struct device *dev, struct device_attribute *att, const char *buf, size_t sz)
{
struct fpga_prog_dev *prg = container_of( dev, struct fpga_prog_dev, dev );
int    err;

	if ( prg->file ) {
		kfree( prg->file );
	}

	prg->file = kstrdup( buf, GFP_KERNEL );
	if ( ! prg->file ) {
		return -ENOMEM;
	} else {
		if ( prg->autoload && (err = load_fw( prg ) ) ) {
			sz = err;
		}
	}
	return sz;
}

static ssize_t
file_show(struct device *dev, struct device_attribute *att, char *buf)
{
struct fpga_prog_dev *prg = container_of( dev, struct fpga_prog_dev, dev );
int                   len;

	if ( ! prg->file ) {
		buf[0] = 0;
		len    = 0;
	} else {
		len = snprintf(buf, PAGE_SIZE, "%s", prg->file);
		if ( len >= PAGE_SIZE )
			len = PAGE_SIZE - 1;
	}
	return len;
}

static ssize_t
program_store(struct device *dev, struct device_attribute *att, const char *buf, size_t sz)
{
struct fpga_prog_dev *prg = container_of( dev, struct fpga_prog_dev, dev );
int                   val;
int                   err;

	if ( kstrtoint(buf, 0, &val) ) {
		return -EINVAL;
	}

	if ( val ) {
		if ( (err = load_fw( prg )) ) {
			sz = err;
		}
	}
	return sz;
}

static ssize_t
autoload_show(struct device *dev, struct device_attribute *att, char *buf)
{
struct fpga_prog_dev *prg = container_of( dev, struct fpga_prog_dev, dev );

	/* dont see how that can overflow PAGE_SIZE */
	return snprintf(buf, PAGE_SIZE, "%d\n", prg->autoload);
}

static ssize_t
autoload_store(struct device *dev, struct device_attribute *att, const char *buf, size_t sz)
{
struct fpga_prog_dev *prg = container_of( dev, struct fpga_prog_dev, dev );

	if ( kstrtoint(buf, 0, &prg->autoload) ) {
		return -EINVAL;
	}

	return sz;
}

static int __init
oftst_init(void)
{
int                    err     = 0;
int                    a_0_err = -1;

	fpga_prog_class = class_create( THIS_MODULE, "fpga_prog" );
	if ( IS_ERR( fpga_prog_class ) ) {
		err = PTR_ERR( fpga_prog_class );
		printk(KERN_ERR "%s: unable to create class (%i)\n", class_name, err);
		goto bail;
	}

	if ( (a_0_err = class_create_file( fpga_prog_class, &class_attr_bind )) ) {
		printk(KERN_ERR "%s: unable to create class file (%i)\n", class_name, a_0_err);
		goto bail;
	}

/*
struct device_node     *nod;
struct fpga_manager    *mgr;
const char          *path = "/amba/devcfg@f8007000";
struct fpga_image_info info;
	info.flags                      = 0;
	info.enable_timeout_us          = 1000000;
	info.disable_timeout_us         = 1000000;
	info.config_complete_timeout_us = 1000000;

	nod = of_find_node_by_path( path );

    if ( nod ) {
		mgr = of_fpga_mgr_get( nod );
		if ( IS_ERR( mgr ) ) {
			printk(KERN_DEBUG "Found no FPGA manager (%li)\n", PTR_ERR( mgr ));
		} else {
			printk(KERN_DEBUG "Found FPGA manager: %s\n", mgr->name);

            err = fpga_mgr_firmware_load( mgr, &info , "/mnt/zybo_generic.bin");

            fpga_mgr_put( mgr );

			printk(KERN_DEBUG "Writing firmware returned %i\n", err);
		}
	} else {
		printk(KERN_ERR "Device (%s) not found\n", path);
	}

	printk(KERN_DEBUG "Hello %p\n", nod);
*/

	return 0;

bail:

	if ( ! a_0_err ) {
		class_remove_file( fpga_prog_class, &class_attr_bind );
	}

	if ( ! IS_ERR( fpga_prog_class ) ) {
		class_destroy( fpga_prog_class );
	}

	return err;
}

static int del_class_dev(struct device *dev, void *data)
{
	device_unregister( dev );
	return 0;
}

static void
oftst_exit(void)
{
	class_for_each_device( fpga_prog_class, 0, 0, del_class_dev );
	/* Removing individual files is probably not necessary ? */
	class_remove_file( fpga_prog_class, &class_attr_bind );
	class_destroy( fpga_prog_class );
}

module_init( oftst_init );
module_exit( oftst_exit );
