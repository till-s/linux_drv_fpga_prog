#include <linux/module.h>
#include <linux/printk.h>
#include <linux/device.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/fpga/fpga-mgr.h>

MODULE_LICENSE("Dual BSD/GPL");

struct fpga_prog_drvdat;
static struct platform_driver fpga_prog_driver;

#define OF_COMPAT "tills,fpga-programmer-1.0"

static const char *drvnam = "prog-fpga";

static DEFINE_IDA(fpga_prog_ida);

static ssize_t
add_prog_store(struct device_driver *drv, const char *buf, size_t sz);

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

static int
fpga_prog_probe(struct platform_device *pdev);
static int
fpga_prog_remove(struct platform_device *dev);

static struct fpga_prog_drvdat *
get_drvdat(struct device *dev);

static void
release_drvdat(struct fpga_prog_drvdat *prg);

static void release_dev(struct device *dev);

static int load_fw(struct fpga_prog_drvdat *prg);

DRIVER_ATTR( add_programmer, S_IWUSR | S_IWGRP, 0, add_prog_store );

DEVICE_ATTR_WO( remove   );
DEVICE_ATTR_RW( file     );
DEVICE_ATTR_WO( program  );
DEVICE_ATTR_RW( autoload );

static struct device_attribute *dev_attrs[] = {
	&dev_attr_program,
	&dev_attr_file,
	&dev_attr_autoload,
};

#define N_DEV_ATTRS (sizeof(dev_attrs)/sizeof(dev_attrs[0]))

#define MAXLEN 1023

struct fpga_prog_dev {
	struct platform_device pdev;
	struct device_node    *mgrNode;
};

struct fpga_prog_drvdat {
	struct platform_device *pdev;
	struct device_node     *mgrNode;
	const char             *file;
	int                    autoload;
	struct fpga_image_info info;
};

static struct fpga_manager *of_get_mgr_from_pdev(struct platform_device *pdev, struct device_node **mgrNode)
{
struct device_node   *pnod = pdev->dev.of_node;
struct device_node   *mnod = 0;
struct fpga_manager  *mgr  = ERR_PTR( -ENODEV );
struct fpga_prog_dev *prgd;

	if ( ! pnod ) {
		/* this is a fpga_prog_dev (run-time created; no OF) */
printk( KERN_ERR " initial pnod NULL; trying parent \n");
		prgd = container_of( pdev, struct fpga_prog_dev, pdev );
		mnod = prgd->mgrNode;
		of_node_get( mnod );
	} else {
		of_node_get( pnod );
			if ( of_device_is_compatible(pnod, OF_COMPAT) ) {
	printk( KERN_ERR " still pnod NULL; trying parent \n");
				mnod = of_parse_phandle(pnod, "fpga-mgr", 0);
			}
		of_node_put( pnod );
	}
	if ( mnod ) {
		mgr = of_fpga_mgr_get( mnod );
		if ( IS_ERR( mgr ) ) {
			of_node_put( mnod );
			mnod = 0;
		}
	} else {
		printk( KERN_ERR "%s: invalid or missing 'fpga-mgr' property\n", drvnam);
		mgr = ERR_PTR( -EINVAL );
	}
	*mgrNode = mnod;
	return mgr;
}


/* Return a manager; reference to mgr->of_node is incremented on success (only) */
static struct fpga_manager *of_get_mgr_from_path(const char *path, struct device_node **mgrNode)
{
struct device_node  *mnod = of_find_node_by_path( path );
struct fpga_manager *mgr  = 0;

	if ( ! mnod )
		return ERR_PTR( -ENOENT );

	mgr = of_fpga_mgr_get( mnod );

	if ( IS_ERR( mgr ) ) {
		of_node_put( mnod );
	} else {
		*mgrNode = mnod;
	}

	return mgr;
}

static int cmp_mgr_node(struct device *dev, void *data)
{
struct fpga_prog_drvdat *prg = get_drvdat( dev );

	return (void *)prg->mgrNode == data;
}

static int cmp_release_func(struct device *dev, void *data)
{
	return dev->release == release_dev && cmp_mgr_node( dev, data );
}

static int load_fw(struct fpga_prog_drvdat *prg)
{
struct fpga_manager   *mgr;
int                    err;
struct device_node    *pnod;

	if ( ! prg->file )
		return -EINVAL;

	/* Platform devices created by this driver don't have an OF-node but
	 * their parent does point to the correct one...
	 */
	pnod = prg->pdev->dev.of_node;
	if ( ! pnod && prg->pdev->dev.parent )
		pnod = prg->pdev->dev.parent->of_node;

	mgr = of_fpga_mgr_get( pnod );

	if ( IS_ERR( mgr ) ) {
		err = PTR_ERR( mgr );
	} else {
		err = fpga_mgr_firmware_load( mgr, &prg->info , prg->file );
		fpga_mgr_put( mgr );
	}

	return err;
}


static void release_dev(struct device *dev)
{
struct fpga_prog_dev *pdev = container_of( dev, struct fpga_prog_dev, pdev.dev );
	printk(KERN_DEBUG "DEV_REL\n");
	of_node_put( pdev->mgrNode );
	ida_simple_remove( &fpga_prog_ida, dev->id );
	kfree( dev );
	module_put( THIS_MODULE );
}

static struct fpga_prog_dev *create_pdev(struct fpga_manager *mgr, struct device_node *mgrNode)
{
struct fpga_prog_dev   *prgd = 0;
struct fpga_prog_dev   *mem  = 0;
int                     id   = -1;
int                     stat;
int                     mgrPut = 0;

	if ( ! ( prgd = kzalloc( sizeof(*prgd), GFP_KERNEL ) ) ) {
		prgd = ERR_PTR( -ENOMEM );
		goto bail;
	}

	mem = prgd;

	prgd->pdev.name = "prog-fpga";

	id = ida_simple_get( &fpga_prog_ida, 0, 0, GFP_KERNEL );
	if ( id < 0 ) {
		prgd = ERR_PTR( id );
		goto bail;
	}

	prgd->pdev.id           = mgr->dev.id;

	prgd->pdev.dev.parent   = mgr->dev.parent;
	prgd->pdev.dev.id       = id;
	prgd->pdev.dev.release  = release_dev;

	prgd->mgrNode           = mgrNode;
	mgrNode                 = 0;

	fpga_mgr_put( mgr );
	mgrPut = 1;


	stat = platform_device_register( &prgd->pdev );
	if ( stat ) {
		prgd = ERR_PTR( stat );
		goto bail;
	}

	/* After this point 'platform_device_unregister()' should take care
	 * of the memory and id
	 */

	mem =  0;
	id  = -1;

	if ( (stat = device_create_file( &prgd->pdev.dev, &dev_attr_remove )) ) {
		platform_device_unregister( &prgd->pdev );
		prgd = ERR_PTR( stat );
		goto bail;
	}

	__module_get( THIS_MODULE );


bail:
	if ( ! mgrPut ) {
		fpga_mgr_put( mgr );
	}

	if ( id >= 0 ) {
		ida_simple_remove( &fpga_prog_ida, id );
	}

	if ( mgrNode ) {
		of_node_put( mgrNode );
	}

	if ( mem ) {
		of_node_put( mem->mgrNode );
		kfree( mem );
		module_put( THIS_MODULE );
	}

	return prgd;
}

static struct fpga_prog_drvdat *create_drvdat( struct platform_device *pdev, struct device_node *mgrNode )
{
struct fpga_prog_drvdat *prog;
struct device           *dev;
struct device_node      *pnod;
int                      stat;
const char              *str;
u32                      val;

	if ( (dev = driver_find_device( &fpga_prog_driver.driver, 0, mgrNode, cmp_mgr_node )) ) {
		put_device( dev );
		return ERR_PTR(-EEXIST);
	}

	if ( ! ( prog = kzalloc( sizeof(*prog), GFP_KERNEL ) ) ) {
		return ERR_PTR(-ENOMEM);
	}

	prog->pdev                            = pdev;
	prog->mgrNode                         = mgrNode;
	prog->autoload                        = 1;

	prog->info.flags                      = 0;
	prog->info.enable_timeout_us          = 1000000;
	prog->info.disable_timeout_us         = 1000000;
	prog->info.config_complete_timeout_us = 1000000;

	if ( (pnod = pdev->dev.of_node) ) {
		/* try to load parameters from OF */
		of_node_get( pnod );

		stat = of_property_read_string( pnod, "file", &str );
		if ( 0 == stat ) {
			prog->file = kstrdup( str, GFP_KERNEL );
		} else if ( stat != -EINVAL ) {
			printk(KERN_WARNING "%s: unable to read 'file' property from OF (%d)\n", drvnam, stat);
		}

		stat = of_property_read_u32( pnod, "autoload", &val );
		if ( 0 == stat ) {
			prog->autoload = val;
		} else if ( stat != -EINVAL ) {
			printk(KERN_WARNING "%s: unable to read 'autload' property from OF (%d)\n", drvnam, stat);
		}
			
		of_node_put( pnod );
	}

	return prog;
}

static int dev_attach(struct platform_device *pdev, struct fpga_manager *mgr, struct device_node *mgrNode)
{
struct fpga_prog_drvdat *drvdat;
struct fpga_prog_drvdat *mem  = 0;
int                      stat = 0;
int                      dev_attr_stat[ N_DEV_ATTRS ];
int                      i;
struct kobject          *mgrObj = 0;

	for ( i=0; i<N_DEV_ATTRS; i++ ) {
		dev_attr_stat[i] = -1;
	}

	drvdat = create_drvdat( pdev, mgrNode );

	if ( IS_ERR( drvdat ) ) {
		of_node_put( mgrNode );
		stat = PTR_ERR( drvdat );
		goto bail;
	}

	mem = drvdat;

	/* for any error after here the mgrNode is 'put' by release_drvdat */

	drvdat->pdev = pdev;
	platform_set_drvdata( pdev, drvdat );

	for ( i=0; i<N_DEV_ATTRS; i++ ) {
		if ( (dev_attr_stat[i] = device_create_file( &pdev->dev, dev_attrs[i] )) )
			break;
	}
	if ( i < N_DEV_ATTRS ) {
		stat = dev_attr_stat[i];
		goto bail;
	}

	mgrObj = & mgr->dev.kobj;	

	kobject_get( mgrObj );

	stat = sysfs_create_link( &pdev->dev.kobj, mgrObj, "fpga_manager" );

	kobject_put( mgrObj );

	if ( stat ) {
		goto bail;
	}

	dev_attr_stat[0] = -1;
	mem              = 0;

bail:
	for ( i=0; i < N_DEV_ATTRS && dev_attr_stat[i] == 0; i++ ) {
		device_remove_file( &pdev->dev, dev_attrs[i] );
	}

	if ( mem )
		release_drvdat( mem );

	return stat;
}

static int fpga_prog_probe(struct platform_device *pdev)
{
struct fpga_manager     *mgr;
struct device_node      *mgrNode;
int                      stat, fwstat;
struct fpga_prog_drvdat *prg;

	printk("PROG_PROBE\n");

	mgr = of_get_mgr_from_pdev( pdev, &mgrNode );
	if ( IS_ERR( mgr ) ) {
		printk(KERN_ERR "%s: no fpga-manager found (%ld)\n", drvnam, PTR_ERR(mgr));
		return PTR_ERR( mgr );
	}

	/* dev_attach() 'consumes' the reference to mgrNode -
	 * either by storing in drvdat or releasing it on failure
	 */
	stat = dev_attach( pdev, mgr, mgrNode );

	if ( 0 == stat ) {
		prg = platform_get_drvdata( pdev );
		if ( prg->file && prg->autoload ) {
			fwstat = fpga_mgr_firmware_load( mgr, &prg->info , prg->file );
			if ( fwstat ) {
				printk(KERN_WARNING "%s: programming firmware failed (%d)\n", drvnam, fwstat);
			}
		}
	}

	fpga_mgr_put( mgr );

	return stat;
}

static void release_drvdat(struct fpga_prog_drvdat *prg)
{
	if ( prg->mgrNode ) {
		of_node_put( prg->mgrNode );
	}

	if ( prg->file ) {
		kfree( prg->file );
	}

	kfree( prg );
}

static int fpga_prog_remove(struct platform_device *pdev)
{
struct fpga_prog_drvdat *prg = platform_get_drvdata( pdev );
int                      i;

	printk("PROG_RELEASE\n");

	for ( i=0; i < N_DEV_ATTRS; i++ ) {
		device_remove_file( &pdev->dev, dev_attrs[i] );
	}

	sysfs_remove_link( &pdev->dev.kobj, "fpga_manager" );

	release_drvdat( prg );

	return 0;
}

static ssize_t
add_prog_store(struct device_driver *drv, const char *buf, size_t sz)
{
struct fpga_manager     *mgr;
struct fpga_prog_dev    *pdev;
struct device           *dev;
struct device_node      *mgrNode = 0;

	if ( buf[sz] )
		return -EINVAL;


	if ( sz > MAXLEN ) {
		return -ENOMEM;
	}

	mgr = of_get_mgr_from_path( buf, &mgrNode );

	if ( IS_ERR( mgr ) ) {
		return PTR_ERR( mgr );
	}

	/** FIXME **/
	if ( (dev = device_find_child( &platform_bus, mgrNode , cmp_release_func )) ) {
		put_device( dev );
		fpga_mgr_put( mgr );
		pdev = ERR_PTR(-EEXIST);
	} else {
		/* create_pdev releases the manager and keeps a ref. to the mgrNode
		 * this ref. either lives on inside the pdev or must be released here
		 * if create_pdev fails.
		 */
		pdev = create_pdev( mgr, mgrNode );
	}

	if ( IS_ERR( pdev ) ) {
		sz = PTR_ERR( pdev );
		of_node_put( mgrNode );
	}

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

static struct fpga_prog_drvdat *get_drvdat(struct device *dev)
{
struct platform_device *pdev = container_of( dev, struct platform_device, dev );
	return (struct fpga_prog_drvdat*) platform_get_drvdata( pdev );
}

static ssize_t
file_store(struct device *dev, struct device_attribute *att, const char *buf, size_t sz)
{
struct fpga_prog_drvdat *prg = get_drvdat( dev );
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
struct fpga_prog_drvdat *prg = get_drvdat( dev );
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
struct fpga_prog_drvdat *prg = get_drvdat( dev );
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
struct fpga_prog_drvdat *prg = get_drvdat( dev );

	/* dont see how that can overflow PAGE_SIZE */
	return snprintf(buf, PAGE_SIZE, "%d\n", prg->autoload);
}

static ssize_t
autoload_store(struct device *dev, struct device_attribute *att, const char *buf, size_t sz)
{
struct fpga_prog_drvdat *prg = get_drvdat( dev );

	if ( kstrtoint(buf, 0, &prg->autoload) ) {
		return -EINVAL;
	}

	return sz;
}

#ifdef CONFIG_OF
static const struct of_device_id fpga_prog_of_match[] = {
	{ .compatible = OF_COMPAT, },
	{},
};

MODULE_DEVICE_TABLE(of, fpga_prog_of_match);
#endif

static struct platform_device_id fpga_prog_ids[] = {
	{ .name = "prog-fpga" },
	{}
};

static struct platform_driver fpga_prog_driver = {
	.driver = {
		.name           = "fpga_programmer",
		.owner          = THIS_MODULE,
		.of_match_table = of_match_ptr( fpga_prog_of_match )
	},
	.id_table = fpga_prog_ids,
	.probe    = fpga_prog_probe,
	.remove   = fpga_prog_remove
};

static int __init
oftst_init(void)
{
int                    err     = 0;

	err = platform_driver_register( &fpga_prog_driver );

	if ( ! err ) {
		if ( (err = driver_create_file( &fpga_prog_driver.driver, &driver_attr_add_programmer )) ) {
			platform_driver_unregister( &fpga_prog_driver );
		}
	}

	return err;
}

static void
oftst_exit(void)
{
	platform_driver_unregister( &fpga_prog_driver );
}

module_init( oftst_init );
module_exit( oftst_exit );
